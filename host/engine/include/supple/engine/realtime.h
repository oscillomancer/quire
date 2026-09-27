// The real-time engine (spec: docs/specs/engine.md §1.6, §2.3): plays a
// session through an audio backend. The main thread compiles a session
// version into a plan and hands it to the audio thread (RtHandoff); the audio
// thread adopts it at the next cycle, starts processing new plugin instances
// and stops removed ones; retired plans and unused instances are freed on the
// main thread (service()). Transport commands travel on a wait-free queue.
//
// This first cut runs every node inside the audio callback (no anticipative
// processing yet). Its output is bit-identical to the offline renderer for
// block-size-invariant plugins; it lags the render position by the plan's
// output latency (the transport reports the render position).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/handoff.h"
#include "supple/base/spsc.h"
#include "supple/engine/offline.h"

namespace supple::engine {

namespace detail {
struct Plan;
}

struct BackendConfig {
    double sampleRate = 48000.0;
    std::uint32_t blockSize = 256;
    std::uint32_t inputs = 0, outputs = 2;
};

// What a running backend reports (for engine.status).
struct BackendStatus {
    std::uint32_t period = 0;         // the device's period, in frames
    std::uint64_t xruns = 0;          // reported by the device
    std::uint32_t outputLatency = 0;  // frames from the backend's outputs to the device
    bool restartNeeded = false;       // the device's rate changed or it went away
};

// An audio device driver. The callback runs on the backend's audio thread.
// Contract for stop(): after the last audio cycle and before stop() returns,
// the backend calls the callback once more with frames == 0 on its audio
// thread (a Manual driver: on the caller, which is its audio thread), so the
// engine can stop every plugin where CLAP requires it.
class Backend {
  public:
    using Callback =
        std::function<void(const float* const* in, float* const* out, std::uint32_t frames)>;
    virtual ~Backend() = default;
    virtual Status start(const BackendConfig& config, Callback callback) = 0;
    virtual void stop() = 0;
    virtual BackendConfig config() const = 0;
    virtual std::string name() const = 0;
    // True when callbacks run on a thread of the backend's own (a device);
    // false for a backend driven by its caller.
    virtual bool ownsThread() const = 0;
    virtual BackendStatus status() const { return {config().blockSize, 0, 0, false}; }
};

// No device. `Manual` runs cycles on the caller's thread (deterministic
// tests); `Thread` runs them on its own thread, paced like a device or free.
// Either can capture the output.
class DummyBackend : public Backend {
  public:
    enum class Mode { Manual, Thread };
    struct Options {
        Mode mode = Mode::Manual;
        bool paced = false;              // Thread mode: sleep one block period per cycle
        std::int64_t captureFrames = 0;  // frames of output to keep
    };
    explicit DummyBackend(Options options);
    ~DummyBackend() override;

    Status start(const BackendConfig& config, Callback callback) override;
    void stop() override;
    BackendConfig config() const override { return config_; }
    std::string name() const override { return "dummy"; }
    bool ownsThread() const override { return options_.mode == Mode::Thread; }

    void run(std::int64_t cycles);  // Manual mode: run cycles now, on this thread
    std::int64_t cycles() const { return cycles_.load(); }
    const audio::AudioData& capture() const { return capture_; }  // read after stop() / run()

  private:
    void cycle();
    Options options_;
    BackendConfig config_;
    Callback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> cycles_{0};
    std::vector<float> outL_, outR_;
    audio::AudioData capture_;
    std::int64_t captured_ = 0;
};

struct TransportState {
    bool playing = false;
    std::int64_t sample = 0;  // render position
    double seconds = 0.0;
};

class Engine {
  public:
    Engine(InstancePool& plugins, MediaCache& media);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Starts the backend with the engine's callback.
    Status start(Backend& backend, const BackendConfig& config);
    // Stops processing on every instance (on the audio thread), then the backend.
    void stop();
    bool running() const { return backend_ != nullptr; }

    // Compiles `session` and hands the plan to the audio thread.
    Status load(const model::Session& session, const std::filesystem::path& projectDir = {});
    void play();
    void pause();  // stops the transport; plugin state is reset (no hanging notes)
    void seek(double seconds);
    TransportState transport() const;

    // Main thread, regularly: frees retired plans; once the newest plan is
    // live, destroys instances whose fx left the session.
    void service();

    std::uint32_t outputLatency() const { return outputLatency_; }
    const std::vector<std::string>& warnings() const { return warnings_; }
    std::uint64_t processErrors() const { return processErrors_.load(); }
    // Audio cycles run since start (the main thread's way to see a request acted on).
    std::uint64_t cycles() const { return cycles_.load(std::memory_order_acquire); }
    const BackendConfig& config() const { return config_; }
    // True when the last stop() stopped the plugins in the backend's final
    // (audio-thread) callback, as the Backend contract promises.
    bool lastStopOnAudioThread() const { return stoppedOnAudioThread_.load(); }

  private:
    void cycle(const float* const* in, float* const* out, std::uint32_t frames) SUPPLE_RT;
    void adopt(detail::Plan* next) SUPPLE_RT;
    void resetInstances(detail::Plan* plan) SUPPLE_RT;

    InstancePool& plugins_;
    MediaCache& media_;
    Backend* backend_ = nullptr;
    BackendConfig config_;
    RtHandoff<detail::Plan> handoff_;
    // Transport requests: coalescing atomics (the latest request wins and none
    // is ever dropped), read by the audio thread at the start of each cycle.
    std::atomic<bool> wantPlaying_{false};
    std::atomic<std::int64_t> seekSample_{0};
    std::atomic<std::uint64_t> seekSeq_{0};
    std::atomic<bool> shutdownRequested_{false};
    std::uint64_t seekSeen_ = 0;  // audio thread
    // Plans carry a generation; the audio thread stores it here after adopt()
    // has stopped the instances the plan dropped. service() prunes only then.
    std::uint64_t publishedGen_ = 0;
    std::atomic<std::uint64_t> stoppedGen_{0};
    std::shared_ptr<const model::Session> latest_;
    bool prunePending_ = false;
    std::uint32_t outputLatency_ = 0;
    std::vector<std::string> warnings_;

    // Audio-thread state. The previous plan is handed back to the main thread
    // the moment a new one is acquired, so the audio thread keeps its own list
    // of the instances it is processing (preallocated; never grows there).
    static constexpr std::size_t kMaxInstances = 4096;
    detail::Plan* plan_ = nullptr;
    std::vector<host::Instance*> processing_;
    bool playing_ = false;
    std::int64_t pos_ = 0;
    // Shared with the main thread.
    std::atomic<std::int64_t> reportedPos_{0};
    std::atomic<bool> reportedPlaying_{false};
    std::atomic<bool> shutDown_{false};
    std::atomic<bool> stoppedOnAudioThread_{false};
    std::atomic<std::uint64_t> processErrors_{0};
    std::atomic<std::uint64_t> cycles_{0};
};

}  // namespace supple::engine
