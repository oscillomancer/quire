#include "supple/engine/offline.h"

#include "plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>
#include <set>
#include <thread>

#include "supple/clap/semantic_params.h"
#include "supple/time/timeline.h"

namespace supple::engine {

using model::Id;
using model::Kind;
using model::Session;

namespace {
// Keep the target and precisely the tracks whose enabled sends feed it.
Session upstreamSession(const Session& source, Id target) {
    if (target == model::kMaster) return source;
    Session s = source;
    std::set<Id> keep{target};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& entry : source.tracks) {
            const auto& track = *entry.value;
            if (track.parentSend.enabled && track.parent && keep.count(*track.parent))
                changed |= keep.insert(entry.id).second;
        }
        for (const auto& entry : source.routes) {
            const auto& route = *entry.value;
            if (!route.mute && route.from.track && route.to.track && keep.count(*route.to.track))
                changed |= keep.insert(*route.from.track).second;
        }
    }
    const bool masterFeedsTarget = keep.count(model::kMaster) != 0;
    keep.insert(model::kMaster);
    // A target's sends toward the master are irrelevant to its tap. Retain a
    // harmless root connection so the compiler still orders the master last.
    model::IdMap<model::Track>::Builder tracks(source.tracks);
    for (const auto& entry : source.tracks) {
        if (!keep.count(entry.id)) {
            tracks.erase(entry.id);
            continue;
        }
        if (entry.id == model::kMaster && !masterFeedsTarget) {
            auto master = std::make_shared<model::Track>(*entry.value);
            master->chain.clear();
            master->mute = false;
            tracks.put(entry.id, entry.rev, std::move(master));
        } else if (!entry.value->parent || !keep.count(*entry.value->parent)) {
            auto track = std::make_shared<model::Track>(*entry.value);
            track->parent = model::kMaster;
            track->parentSend.enabled = true;
            tracks.put(entry.id, entry.rev, std::move(track));
        }
    }
    s.tracks = tracks.build();
    model::IdMap<model::Item>::Builder items(source.items);
    for (const auto& entry : source.items)
        if (!keep.count(entry.value->track) || entry.value->track == model::kMaster)
            items.erase(entry.id);
    s.items = items.build();
    model::IdMap<model::Route>::Builder routes(source.routes);
    for (const auto& entry : source.routes)
        if (!entry.value->from.track || !entry.value->to.track ||
            !keep.count(*entry.value->from.track) || !keep.count(*entry.value->to.track))
            routes.erase(entry.id);
    s.routes = routes.build();
    return s;
}
}  // namespace

// --- instance pool ------------------------------------------------------------------

InstancePool::InstancePool(Resolver resolve, host::HostIdentity identity)
    : resolve_(std::move(resolve)), identity_(std::move(identity)) {}

InstancePool::~InstancePool() = default;

Result<host::Instance*> InstancePool::get(Id id, const model::FxInstance& fx,
                                          const Session& session) {
    const bool isolated = fx.isolation == model::Isolation::SharedBridge ||
                          fx.isolation == model::Isolation::Dedicated;
    std::string desired;
    if (fx.patch)
        desired = "patch:" + json::writeCompact(*fx.patch);
    else if (fx.state)
        desired = "blob:" + *fx.state;
    auto& e = instances_[id];
    // A new state blob is loaded into a fresh instance when the current one
    // may be processing: CLAP's state.load runs on the main thread and a
    // plugin need not guard its DSP against a concurrent process() (semantic
    // patches are designed for live changes and stay in place).
    const bool freshForState = e.instance && !fx.patch && fx.state && desired != e.appliedState &&
                               e.instance->processing();
    if (!e.instance || e.pluginId != fx.plugin.id || e.isolated != isolated || freshForState) {
        // The plan playing now may still use the old instance: keep it alive
        // until prune() says every plan that could use it is gone.
        if (e.instance) retired_.emplace_back(generation_, std::move(e.instance));
        auto info = resolve_(fx.plugin);
        if (!info) {
            instances_.erase(id);
            return std::move(info).error();
        }
        auto made = host::Instance::create(*info, identity_, isolated);
        if (!made) {
            instances_.erase(id);
            return std::move(made).error();
        }
        e.instance = std::move(*made);
        e.pluginId = fx.plugin.id;
        e.isolated = isolated;
        e.appliedState.clear();
    }
    if (desired != e.appliedState && !desired.empty()) {
        if (fx.patch) {
            SUPPLE_TRY(
                e.instance->setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
        } else {
            auto it = session.blobs.find(*fx.state);
            if (it == session.blobs.end())
                return Error("engine.blob_missing", id.str() + " refers to a missing state blob",
                             "/fx/" + id.str() + "/state");
            SUPPLE_TRY(e.instance->loadState(it->second->data));
        }
        e.appliedState = desired;
    }
    return e.instance.get();
}

void InstancePool::prune(const Session& session, std::uint64_t liveGeneration) {
    for (auto it = instances_.begin(); it != instances_.end();)
        it = session.fx.contains(it->first) ? std::next(it) : instances_.erase(it);
    std::erase_if(retired_, [&](const auto& r) { return r.first <= liveGeneration; });
}

void InstancePool::restart(Id id) {
    if (auto it = instances_.find(id); it != instances_.end()) {
        if (it->second.instance) retired_.emplace_back(generation_, std::move(it->second.instance));
        instances_.erase(it);
    }
}

std::vector<std::pair<Id, std::string>> InstancePool::failed() const {
    std::vector<std::pair<Id, std::string>> out;
    for (const auto& [id, e] : instances_)
        if (e.instance && e.instance->failed()) out.emplace_back(id, e.instance->failure());
    return out;
}

host::Instance* InstancePool::find(Id id) const {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.instance.get();
}

// --- media -----------------------------------------------------------------------------

Result<std::shared_ptr<const audio::AudioData>> MediaCache::get(const std::filesystem::path& path,
                                                                double rate) {
    const std::string key = path.lexically_normal().string() + "@" + json::formatDouble(rate);
    auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    auto data = audio::readAudio(path);
    if (!data) return std::move(data).error();
    if (rate > 0 && data->sampleRate != rate) {
        auto resampled = audio::resample(*data, rate);
        if (!resampled) return std::move(resampled).error();
        data = std::move(*resampled);
    }
    auto shared = std::make_shared<const audio::AudioData>(std::move(*data));
    entries_.emplace(key, shared);
    return shared;
}

// --- mixing helpers ---------------------------------------------------------------------

std::pair<double, double> panGains(double pan, double panLawDb) {
    const double p = std::clamp(pan, -1.0, 1.0);
    const double angle = (p + 1.0) * std::numbers::pi / 4.0;
    // The sin/cos curve is constant power: 1/sqrt(2) at the centre, which is
    // what "-3 dB" means. Other laws scale it to their centre gain.
    const double centre =
        panLawDb == -3.0 ? std::numbers::sqrt2 / 2.0 : std::pow(10.0, panLawDb / 20.0);
    const double scale = centre * std::numbers::sqrt2;
    return {std::cos(angle) * scale, std::sin(angle) * scale};
}

using namespace detail;

Result<OfflineResult> renderOffline(const Session& s, InstancePool& pool,
                                    const OfflineSettings& opt) {
    if (!(opt.endSeconds > opt.startSeconds))
        return Error("engine.range", "the render range is empty", {}, "end must be after start");
    if (opt.blockSize < 1 || opt.blockSize > 8192)
        return Error("engine.block_size", "block size out of range", {}, "1 .. 8192");
    if (!(opt.tailSeconds >= 0 && opt.tailSeconds <= 600))
        return Error("engine.tail", "tail out of range", {}, "0 .. 600 s");
    const Session selectedSession = opt.track ? upstreamSession(s, *opt.track) : s;
    auto compiled = detail::compile(selectedSession, pool, opt.startSeconds, opt.blockSize);
    if (!compiled) return std::move(compiled).error();
    Plan& p = *compiled;
    MediaCache localMedia;
    p.projectDir = opt.projectDir;
    p.media = opt.media ? opt.media : &localMedia;
    SUPPLE_TRY(prepare(p, selectedSession));
    const Node* selected = &p.nodes.back();
    if (opt.track) {
        auto it = std::find_if(p.nodes.begin(), p.nodes.end(),
                               [&](const Node& node) { return node.id == *opt.track; });
        if (it == p.nodes.end())
            return Error("track.unknown", "track is absent from the render plan", "/track");
        selected = &*it;
        // An analysis tap is isolated from the session's current solo state.
        for (Node& node : p.nodes) node.soloSilenced = false;
    }
    const auto outputLatency =
        opt.track ? selected->latIn + (opt.tap == model::Tap::PreFx ? 0 : selected->latChain)
                  : p.outputLatency;
    const std::int64_t length = time::samplesAt(opt.endSeconds, p.rate) -
                                time::samplesAt(opt.startSeconds, p.rate) +
                                time::samplesAt(opt.tailSeconds, p.rate);
    const std::int64_t total = length + outputLatency;
    if (total > (std::int64_t(1) << 28)) return Error("engine.too_long", "render too long");
    std::vector<float> outL(static_cast<std::size_t>(total)), outR(static_cast<std::size_t>(total));
    bool failed = false, startFailed = false;

    std::vector<host::Instance*> instances;
    for (Node& n : p.nodes)
        for (Stage& st : n.chain)
            if (st.instance) instances.push_back(st.instance);
    detail::Workers workers(opt.threads == 0 ? detail::Workers::defaultHelpers()
                                             : std::min(opt.threads, 64u) - 1);
    std::thread audio([&] {
        for (host::Instance* in : instances) {
            in->reset();
            in->flushParams();
            if (!in->startProcessing()) startFailed = true;
        }
        if (!startFailed) {
            for (std::int64_t pos = 0; pos < total; pos += p.block) {
                const auto n =
                    static_cast<std::uint32_t>(std::min<std::int64_t>(p.block, total - pos));
                if (!(workers.helpers() ? processBlockOn(workers, p, pos, n)
                                        : processBlock(p, pos, n))) {
                    failed = true;
                    break;
                }
                const Stereo& tap = opt.tap == model::Tap::PreFx      ? selected->preFx
                                    : opt.tap == model::Tap::PreFader ? selected->preFader
                                                                      : selected->post;
                std::copy_n(tap.l.begin(), n, outL.begin() + pos);
                std::copy_n(tap.r.begin(), n, outR.begin() + pos);
            }
        }
        for (host::Instance* in : instances) in->stopProcessing();
    });
    audio.join();

    OfflineResult r;
    for (host::Instance* in : instances) {
        in->serviceMainThread();
        for (auto& line : in->takeLog()) r.log.push_back(in->info().name + ": " + line);
    }
    if (startFailed) return Error("engine.start_failed", "a plugin refused to start processing");
    if (failed) return Error("engine.process_error", "a plugin reported a processing error");
    for (const Node& n : p.nodes) {
        std::string line = n.id.str() + " in=[";
        for (std::size_t k = 0; k < n.in.size(); ++k) {
            const Edge& e = p.edges[n.in[k]];
            static constexpr const char* kTaps[] = {"post_fader", "pre_fader", "pre_fx"};
            char g[64];
            std::snprintf(g, sizeof g, " d%lld g%.3f/%.3f", static_cast<long long>(e.delay.length),
                          e.gainL, e.gainR);
            line += (k ? ", " : "") + p.nodes[e.from].id.str() + " " +
                    kTaps[static_cast<int>(e.tap)] + g;
        }
        line += "] lat_in=" + std::to_string(n.latIn) + " lat_chain=" + std::to_string(n.latChain);
        r.graph.push_back(std::move(line));
    }
    r.outputLatency = static_cast<std::uint32_t>(outputLatency);
    r.warnings = std::move(p.warnings);
    r.audio.sampleRate = p.rate;
    const auto skip = static_cast<std::ptrdiff_t>(outputLatency);
    r.audio.channels = {std::vector<float>(outL.begin() + skip, outL.end()),
                        std::vector<float>(outR.begin() + skip, outR.end())};
    return r;
}

}  // namespace supple::engine
