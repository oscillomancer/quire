// B1 capacity: command-built stock chains, a monitored input and master limiter.
#include "session.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "plan.h"
#include "supple/base/environment.h"
#include "supple/base/files.h"
#include "supple/engine/jack.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"

using namespace supple;
using namespace supple::engine;
namespace {
class PacedBackend final : public Backend {
  public:
    Status start(const BackendConfig& config, Callback cb) override {
        config_ = config;
        callback_ = std::move(cb);
        input_.assign(config.blockSize, 0.f);
        left_.assign(config.blockSize, 0.f);
        right_.assign(config.blockSize, 0.f);
        running_ = true;
        thread_ = std::thread([this] {
            const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(config_.blockSize / config_.sampleRate));
            auto next = std::chrono::steady_clock::now();
            const float* in[2] = {input_.data(), input_.data()};
            float* out[2] = {left_.data(), right_.data()};
            while (running_.load()) {
                const auto start = std::chrono::steady_clock::now();
                callback_(in, out, config_.blockSize);
                if (std::chrono::steady_clock::now() - start > period) ++late_;
                ++cycles_;
                next += period;
                // Count scheduler overruns too, without catching up in a busy loop.
                const auto now = std::chrono::steady_clock::now();
                if (now > next + period) {
                    ++late_;
                    next = now;
                }
                std::this_thread::sleep_until(next);
            }
            callback_(in, out, 0);
        });
        return {};
    }
    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "paced"; }
    bool ownsThread() const override { return true; }
    BackendStatus status() const override {
        return {.period = config_.blockSize, .xruns = late_.load()};
    }
    std::uint64_t cycles() const { return cycles_.load(); }
    ~PacedBackend() override { stop(); }

  private:
    BackendConfig config_;
    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> late_{0}, cycles_{0};
    std::vector<float> input_, left_, right_;
};

int fail(const Error& error) {
    std::fprintf(stderr, "%s\n", error.toString().c_str());
    return 1;
}
int stripBench() {
    using namespace supple::engine::detail;
    std::printf("Console 9 engine strips, stereo, 48 kHz, block 512, 2097152 frames/row\n");
    std::printf("| strip | mode | ns/frame | one 48 kHz strip, %% core |\n|---|---|---:|---:|\n");
    for (bool on : {false, true})
        for (bool buss : {false, true}) {
            model::EditTxn edit(model::Session::create({.console9 = on, .seed = 6789}), 1);
            model::Track t;
            t.parent = model::kMaster;
            if (buss) t.type = model::StripType::Buss;
            const Id target = edit.create(t);
            edit.modify<model::Track>(model::kMaster,
                                      [&](auto& m) { m.children.push_back(target); });
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
    // One search/report implementation serves both hosts; exec replaces this
    // process, preserving exit/signal semantics and forwarding the exact argv.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) != "--search") continue;
        std::vector<std::string> args = {"python3", BENCH_DRIVER, "b1", "--engine", argv[0]};
        for (int j = 1; j < argc; ++j) args.emplace_back(argv[j]);
        std::vector<char*> pointers;
        for (auto& arg : args) pointers.push_back(arg.data());
        pointers.push_back(nullptr);
        execvp(pointers[0], pointers.data());
        std::perror("could not start benchmark search driver");
        return 1;
    }
    std::string device = "paced", pluginDir = BENCH_PLUGINS, recipePath = BENCH_RECIPE;
    std::string output, exportPath;
    int tracks = 2;
    double seconds = 300;
    bool anticipative = true;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::puts(
                    "engine-bench [--device paced|jack] [--tracks N] [--seconds 300] "
                    "[--anticipative on|off] [--plugins DIR] [--output JSON] "
                    "[--export-session JSON] [--search --max-tracks 256 --report "
                    "docs/benchmarks/b1.md]");
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("missing option value");
            const std::string value = argv[i];
            if (arg == "--device")
                device = value;
            else if (arg == "--plugins")
                pluginDir = value;
            else if (arg == "--recipe")
                recipePath = value;
            else if (arg == "--output")
                output = value;
            else if (arg == "--export-session")
                exportPath = value;
            else if (arg == "--tracks") {
                std::size_t used;
                tracks = std::stoi(value, &used);
                if (used != value.size()) throw std::invalid_argument("invalid track count");
            } else if (arg == "--seconds") {
                std::size_t used;
                seconds = std::stod(value, &used);
                if (used != value.size()) throw std::invalid_argument("invalid duration");
            } else if (arg == "--anticipative" && (value == "on" || value == "off"))
                anticipative = value == "on";
            else
                throw std::invalid_argument("unknown option/value: " + arg);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s; use --help\n", e.what());
        return 2;
    }
    if ((device != "paced" && device != "jack") || tracks < 1 || tracks > 4096 ||
        !std::isfinite(seconds) || seconds <= 0) {
        std::fprintf(stderr, "invalid device, track count or duration\n");
        return 2;
    }
    if (device == "jack" && !environmentFlagEnabled("SUPPLE_AUDIO_DEVICE_TEST"))
        return fail(Error("bench.opt_in", "JACK requires SUPPLE_AUDIO_DEVICE_TEST=1"));
    auto text = readTextFile(recipePath);
    if (!text) return fail(text.error());
    auto recipe = json::parse(*text);
    if (!recipe) return fail(recipe.error());
    auto app =
        EngineApp::create({.pluginPath = {pluginDir}, .workdir = std::filesystem::current_path()});
    if (!app) return fail(app.error());
    auto built = bench::buildSession(**app, *recipe, tracks);
    if (!built) return fail(built.error());
    if (!exportPath.empty()) {
        auto exported = bench::exportSession(**app);
        if (!exported) return fail(exported.error());
        auto st = writeFileAtomic(exportPath, json::writeCanonical(*exported) + "\n");
        return st ? 0 : fail(st.error());
    }
    PacedBackend paced;
    std::unique_ptr<JackBackend> jack;
    Backend* backend = &paced;
    if (device == "jack") {
        std::fprintf(stderr,
                     "JACK benchmark: request 48 kHz/64 and call jack_set_buffer_size(64); "
                     "this changes PipeWire's graph quantum for all clients during the run. "
                     "Physical stereo capture/playback ports will be connected.\n");
        std::fflush(stderr);
        jack = std::make_unique<JackBackend>(
            JackOptions{.clientName = "supple-capacity-bench", .forceBufferSize = true});
        backend = jack.get();
    }
    auto started =
        (*app)->startEngine(*backend, {.sampleRate = 48000, .blockSize = 64, .inputs = 2},
                            {.anticipative = anticipative});
    if (!started) {
        (*app)->stopEngine();
        return fail(started.error());
    }
    auto* engine = (*app)->engine();
    if (!engine->awaitPlan(30)) {
        (*app)->stopEngine();
        return fail(Error("bench.prepare", "plan adoption timed out"));
    }
    engine->play();
    engine->awaitTransport();
    // Exclude activation/preroll; measure steady state, including loop wraps.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto before = backend->status();
    const auto m0 = engine->anticipationMisses(), e0 = engine->processErrors();
    const auto c0 = jack ? jack->stats().cycles : paced.cycles();
    const auto begin = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    const double measured =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto after = backend->status();
    auto result = json::Value::object();
    result["schema"] = "supple.capacity-run/1.0";
    result["tracks"] = tracks;
    result["device"] = device;
    result["anticipative"] = anticipative;
    result["seconds"] = measured;
    result["sample_rate"] = backend->config().sampleRate;
    result["quantum"] = after.period;
    result["xruns"] = after.xruns - before.xruns;
    result["anticipation_misses"] = engine->anticipationMisses() - m0;
    result["process_errors"] = engine->processErrors() - e0;
    result["cycles"] = (jack ? jack->stats().cycles : paced.cycles()) - c0;
    result["restart_needed"] = after.restartNeeded;
    result["warnings"] = json::Value::array();
    for (const auto& warning : engine->warnings()) result["warnings"].push_back(warning);
    const bool valid = result["cycles"].asInt() > 0 && !after.restartNeeded &&
                       backend->config().sampleRate == 48000 && after.period == 64 &&
                       engine->processErrors() == 0 && engine->warnings().empty();
    result["valid"] = valid;
    (*app)->stopEngine();
    const auto serialized = json::writeCanonical(result) + "\n";
    if (!output.empty()) {
        auto st = writeFileAtomic(output, serialized);
        if (!st) return fail(st.error());
    }
    std::printf("%s", serialized.c_str());
    return valid ? 0 : 1;
}
