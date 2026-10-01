// Engine capacity benchmark (engine.md §7 B1, preliminary: Orrery only — the
// stock EQ and compressor are not built yet). N tracks, each an Orrery
// instance with the synth.md §9 reference patch holding an 8-note chord
// (3-voice unison), measured three ways:
//   1. offline throughput, one thread vs the worker pool (real-time factor);
//   2. real time, everything in the callback, at the device block: callbacks
//      whose processing took longer than their period (deadline misses);
//   3. real time, anticipative: the callback copies rendered audio; misses
//      are cycles that found nothing rendered.
// A paced thread stands in for the device (no audio device is opened).
//   engine-bench <orrery-headless.clap> [--tracks 4,8,16] [--seconds 5] [--block 64]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "plan.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

constexpr const char* kPatch = R"({"schema": "supple.orrery-patch/1.0",
  "voice": {"polyphony": 8, "unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %", "quality": "normal"},
  "oscillators": {
    "osc1": {"type": "va", "wave": "saw", "level": "-6 dB", "to": "filter1"},
    "osc2": {"type": "va", "wave": "square", "level": "-6 dB", "fine": "+7 ct", "to": "filter1"},
    "osc3": {"type": "sub", "wave": "sine", "octave": -1, "level": "-12 dB", "to": "filter1"}},
  "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "30 %"}},
  "drive": {"type": "tape", "amount": "6 dB"},
  "envelopes": {"amp": {"attack": "10 ms", "sustain": "80 %", "release": "400 ms"},
                "env2": {"attack": "200 ms", "decay": "1 s", "sustain": "20 %"}},
  "lfos": {"lfo1": {"rate": "0.3 Hz"}},
  "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}, "lfo1>osc1.fine": {"amount": "10 ct"}},
  "fx": {"chain": ["chorus", "reverb"]}})";

model::Session session(int tracks, double seconds) {
    model::EditTxn t(model::Session::create(), 1);
    const auto beats = static_cast<std::int64_t>(seconds * 2.0) + 2;  // 120 BPM
    for (int k = 0; k < tracks; ++k) {
        model::Track tr;
        tr.name = "Orrery " + std::to_string(k + 1);
        tr.parent = kMaster;
        tr.volumeDb = -18.0;
        const Id track = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
        model::FxInstance fx;
        fx.plugin.id = "com.suppleaudio.orrery";
        fx.patch = json::parse(kPatch).value();
        const Id f = t.create(fx);
        t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = beats * 960;
        for (int key : {48, 52, 55, 59, 62, 64, 67, 71})
            src.sequence.notes.push_back(
                {0, beats * 960, 0, static_cast<std::uint8_t>(key + k % 5), 90, 0});
        const Id mi = t.create(src);
        model::Take take;
        take.source = mi;
        const Id tk = t.create(take);
        model::Item item;
        item.track = track;
        item.length = static_cast<double>(beats);
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    return t.commit().value();
}

// A device stand-in: calls back every block period on its own thread and
// counts callbacks that took longer than their period.
class PacedBackend : public Backend {
  public:
    Status start(const BackendConfig& config, Callback cb) override {
        config_ = config;
        callback_ = std::move(cb);
        l_.assign(config.blockSize, 0.0f);
        r_.assign(config.blockSize, 0.0f);
        running_ = true;
        thread_ = std::thread([this] {
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(config_.blockSize / config_.sampleRate));
            auto next = std::chrono::steady_clock::now();
            float* out[2] = {l_.data(), r_.data()};
            while (running_.load()) {
                const auto t0 = std::chrono::steady_clock::now();
                callback_(nullptr, out, config_.blockSize);
                if (std::chrono::steady_clock::now() - t0 > period) late_.fetch_add(1);
                cycles_.fetch_add(1);
                next += period;
                std::this_thread::sleep_until(next);
            }
            callback_(nullptr, out, 0);
        });
        return okStatus();
    }
    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "paced"; }
    bool ownsThread() const override { return true; }
    std::uint64_t late() const { return late_.load(); }
    std::uint64_t cycles() const { return cycles_.load(); }
    ~PacedBackend() override { stop(); }

  private:
    BackendConfig config_;
    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> late_{0}, cycles_{0};
    std::vector<float> l_, r_;
};

struct RealTime {
    std::uint64_t cycles = 0, misses = 0;
};

RealTime runRealTime(const model::Session& s, InstancePool& pool, bool anticipative,
                     std::uint32_t block, double seconds) {
    MediaCache media;
    PacedBackend backend;
    EngineOptions options;
    options.anticipative = anticipative;
    Engine engine(pool, media, options);
    if (!engine.start(backend, {.sampleRate = 48000, .blockSize = block}) || !engine.load(s)) {
        std::fprintf(stderr, "could not start the engine\n");
        std::exit(1);
    }
    if (anticipative) engine.awaitPlan();
    engine.play();
    if (anticipative) engine.awaitRendered(1 << 20, 30.0);
    const auto c0 = backend.cycles();
    const auto l0 = backend.late();
    const auto m0 = engine.anticipationMisses();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    RealTime r;
    r.cycles = backend.cycles() - c0;
    r.misses = anticipative ? engine.anticipationMisses() - m0 : backend.late() - l0;
    engine.stop();
    return r;
}

double offlineFactor(const model::Session& s, InstancePool& pool, unsigned threads,
                     double seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = seconds, .threads = threads});
    if (!r) {
        std::fprintf(stderr, "%s\n", r.error().toString().c_str());
        std::exit(1);
    }
    const double spent =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return seconds / spent;  // > 1: faster than real time
}

int stripBench() {
    using namespace supple::engine::detail;
    std::printf("Console 9 engine strips, stereo, 48 kHz, block 512, 2097152 frames/row\n");
    std::printf("| strip | mode | ns/frame | one 48 kHz strip, %% core |\n|---|---|---:|---:|\n");
    for (bool on : {false, true})
        for (bool buss : {false, true}) {
            model::EditTxn edit(model::Session::create({.console9 = on, .seed = 6789}), 1);
            model::Track t;
            t.parent = kMaster;
            if (buss) t.type = model::StripType::Buss;
            const Id target = edit.create(t);
            edit.modify<model::Track>(kMaster, [&](auto& m) { m.children.push_back(target); });
            if (buss) {
                model::Track ch;
                ch.parent = target;
                const Id source = edit.create(ch);
                edit.modify<model::Track>(target, [&](auto& b) { b.children.push_back(source); });
            }
            auto session = edit.commit();
            if (!session) return 1;
            InstancePool pool([](const model::PluginRef&) -> Result<host::PluginInfo> {
                return Error("bench.plugin", "strip benchmark uses no plugins");
            });
            auto compiled = compile(*session, pool, 0, 512);
            if (!compiled || !prepare(*compiled, *session)) return 1;
            auto& p = *compiled;
            const auto at = static_cast<std::size_t>(
                std::find_if(p.nodes.begin(), p.nodes.end(),
                             [&](const auto& n) { return n.id == target; }) -
                p.nodes.begin());
            // A channel's device input, or an already encoded upstream signal for a buss.
            std::vector<float> input(512, .01f);
            const float* in[] = {input.data()};
            p.deviceIn = in;
            p.deviceInChannels = 1;
            if (buss) {
                for (auto& n : p.nodes)
                    if (n.type == model::StripType::Channel) {
                        std::fill(n.post.l.begin(), n.post.l.end(), .0054f);
                        std::fill(n.post.r.begin(), n.post.r.end(), .0054f);
                    }
            } else
                p.nodes[at].monitorInputs = {0};
            const auto transport = transportAt(p, 0);
            for (int i = 0; i < 32; ++i) processNode(p, at, 0, 512, transport);
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 4096; ++i) processNode(p, at, i * 512, 512, transport);
            const double ns =
                std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin)
                    .count() /
                2097152;
            std::printf("| %s | %s | %.2f | %.3f |\n", buss ? "buss" : "channel",
                        on ? "on" : "linear", ns, ns * 48000 / 1e7);
        }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--strips") return stripBench();
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <orrery-headless.clap> [--tracks 4,8] [--seconds 5] [--block 64]\n",
                     argv[0]);
        return 2;
    }
    const std::filesystem::path plugin = argv[1];
    std::vector<int> counts = {2, 4, 8, 12, 16, 24};
    double seconds = 5.0;
    std::uint32_t block = 64;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--tracks") {
            counts.clear();
            std::stringstream ss(argv[i + 1]);
            for (std::string n; std::getline(ss, n, ',');) counts.push_back(std::atoi(n.c_str()));
        } else if (a == "--seconds") {
            seconds = std::atof(argv[i + 1]);
        } else if (a == "--block") {
            block = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
        }
    }
    auto resolve = [&](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&plugin, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("bench.plugin", ref.id + " not found in " + plugin.string());
    };
    std::printf(
        "Engine capacity: Orrery reference patch, 8-note chord per track, 48 kHz, "
        "device block %u, %.0f s per run, %u hardware threads\n\n",
        block, seconds, std::thread::hardware_concurrency());
    std::printf(
        "| tracks | offline x RT, 1 thread | offline x RT, pool | in-callback late cycles | "
        "anticipative misses |\n|---:|---:|---:|---:|---:|\n");
    for (int n : counts) {
        const model::Session s = session(n, seconds + 2.0);
        InstancePool pool(resolve);
        const double one = offlineFactor(s, pool, 1, 3.0);
        const double many = offlineFactor(s, pool, 0, 3.0);
        InstancePool rtPool(resolve);
        const RealTime inCallback = runRealTime(s, rtPool, false, block, seconds);
        InstancePool apPool(resolve);
        const RealTime ahead = runRealTime(s, apPool, true, block, seconds);
        std::printf("| %d | %.2f | %.2f | %llu / %llu | %llu / %llu |\n", n, one, many,
                    static_cast<unsigned long long>(inCallback.misses),
                    static_cast<unsigned long long>(inCallback.cycles),
                    static_cast<unsigned long long>(ahead.misses),
                    static_cast<unsigned long long>(ahead.cycles));
        std::fflush(stdout);
    }
    return 0;
}
