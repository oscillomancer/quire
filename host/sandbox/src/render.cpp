#include "supple/sandbox/render.h"

#include <algorithm>
#include <cmath>
#include <thread>

#include "supple/base/rt.h"
#include "supple/base/thread.h"
#include "supple/dsp/dsp.h"

namespace supple::sandbox {

host::Transport transportAt(const RenderSettings& s, std::int64_t sample) {
    host::Transport t;
    t.playing = true;
    t.tempo = s.tempo;
    t.numerator = s.numerator;
    t.denominator = s.denominator;
    t.seconds = static_cast<double>(sample) / s.sampleRate;
    t.beat = t.seconds * s.tempo / 60.0;
    const double beatsPerBar = s.numerator * 4.0 / s.denominator;
    t.bar = static_cast<std::int32_t>(std::floor(t.beat / beatsPerBar + 1e-12));
    t.barStartBeat = t.bar * beatsPerBar;
    return t;
}

namespace {

// ~11.6 minutes at 48 kHz; 256 MiB of stereo float output.
constexpr std::int64_t kMaxRenderFrames = std::int64_t(1) << 25;

struct Stage {
    host::Instance* instance = nullptr;
    std::vector<float> left, right;  // output buffers (blockSize)
    float* out[2] = {nullptr, nullptr};
    std::vector<host::Event> blockEvents;  // capacity = all events for this target
    std::vector<host::OutputEvent> outStorage;
    host::OutputEvents outEvents;
};

struct Job {
    const RenderSettings* settings = nullptr;
    const RenderRequest* request = nullptr;
    std::vector<TimedEvent> events;  // sorted
    std::vector<Stage> stages;
    std::vector<float> zeroL, zeroR, inL, inR;
    const float* inPtr[2] = {nullptr, nullptr};
    std::vector<float> outL, outR;  // sized to the maximum render length
    std::int64_t length = 0, latency = 0, maxTotal = 0, rendered = 0;
    std::int64_t tailEnd = -1;   // musical end + the chain's reported tail, or -1 (unknown)
    bool tailsReported = false;  // every stage reports its tail (clap.tail)
    std::int64_t silenceHold = 0;
    double silenceLinear = 0;
    bool capped = false, error = false, startFailed = false;
};

// The block loop: runs on the render (audio) thread and must not allocate,
// lock or block. Verified under RealtimeSanitizer by the sandbox tests.
void renderLoop(Job& j) SUPPLE_RT {
    const RenderSettings& s = *j.settings;
    const audio::AudioData* input = j.request->input;
    const std::int64_t inputFrames = input ? static_cast<std::int64_t>(input->frames()) : 0;
    const std::int64_t musicalEnd = j.length + j.latency;
    std::size_t next = 0;
    std::int64_t pos = 0, silentRun = 0;
    // A plug-in's tail can change as it processes (a delay learns the tempo
    // from the transport, parameter changes are adopted on the audio thread):
    // re-read it after every block, so the end follows what it reports now.
    const auto updateTail = [&] {
        if (!j.tailsReported) return;
        std::int64_t tail = 0;
        for (const auto& st : j.stages) {
            const std::uint32_t t = st.instance->tail();
            if (t == UINT32_MAX) {
                j.tailEnd = -1;
                return;
            }
            tail += t;
        }
        j.tailEnd = musicalEnd + tail;
    };
    const auto endNow = [&] {
        return j.tailEnd >= 0 ? std::min(j.maxTotal, j.tailEnd) : j.maxTotal;
    };
    while (pos < endNow()) {
        // Each block runs as an engine node does (ADR-0025), so a chain
        // renders here as it does in a session.
        dsp::DenormalGuard denormals;
        const std::int64_t end = endNow();
        const auto n = static_cast<std::uint32_t>(std::min<std::int64_t>(s.blockSize, end - pos));
        // Chain input for this block.
        const bool haveInput = input && pos < inputFrames;
        if (haveInput) {
            const auto avail =
                static_cast<std::uint32_t>(std::min<std::int64_t>(n, inputFrames - pos));
            const std::size_t lc = 0, rc = input->channelCount() > 1 ? 1 : 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                j.inL[i] =
                    i < avail ? input->channels[lc][static_cast<std::size_t>(pos) + i] : 0.0f;
                j.inR[i] =
                    i < avail ? input->channels[rc][static_cast<std::size_t>(pos) + i] : 0.0f;
            }
            j.inPtr[0] = j.inL.data();
            j.inPtr[1] = j.inR.data();
        } else {
            j.inPtr[0] = j.zeroL.data();
            j.inPtr[1] = j.zeroR.data();
        }
        // Route this block's events to their targets.
        for (auto& st : j.stages) st.blockEvents.clear();
        while (next < j.events.size() && j.events[next].sample < pos + n) {
            const TimedEvent& te = j.events[next++];
            if (te.target >= j.stages.size()) continue;
            host::Event e = te.event;
            e.time = static_cast<std::uint32_t>(std::max<std::int64_t>(0, te.sample - pos));
            j.stages[te.target].blockEvents.push_back(e);  // within reserved capacity
        }
        const host::Transport transport = transportAt(s, pos);
        const float* const* chainIn = j.inPtr;
        for (auto& st : j.stages) {
            host::ProcessBlock b;
            b.inputs = st.instance->audioInputChannels() ? chainIn : nullptr;
            b.inputChannels = st.instance->audioInputChannels() ? 2 : 0;
            b.outputs = st.out;
            b.outputChannels = 2;
            b.frames = n;
            b.events = st.blockEvents;
            b.transport = &transport;
            b.steadyTime = pos;
            st.outEvents.clear();
            if (!st.instance->process(b, &st.outEvents)) {
                j.error = true;
                j.rendered = pos;
                return;
            }
            chainIn = st.out;
        }
        // Collect the chain output; watch for the silent tail.
        float blockPeak = 0.0f;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!std::isfinite(chainIn[0][i]) || !std::isfinite(chainIn[1][i])) {
                j.error = true;
                j.rendered = pos + i;
                return;
            }
            const auto at = static_cast<std::size_t>(pos) + i;
            j.outL[at] = chainIn[0][i];
            j.outR[at] = chainIn[1][i];
            if (pos + i >= musicalEnd)
                blockPeak =
                    std::max({blockPeak, std::fabs(chainIn[0][i]), std::fabs(chainIn[1][i])});
        }
        const std::int64_t blockStart = pos;
        pos += n;
        updateTail();
        if (pos > musicalEnd) {
            // Only the samples past the musical end count toward the silent hold.
            const std::int64_t beyond = pos - std::max(blockStart, musicalEnd);
            silentRun = static_cast<double>(blockPeak) < j.silenceLinear ? silentRun + beyond : 0;
            if (silentRun >= j.silenceHold) break;
        }
    }
    j.capped = pos >= j.maxTotal && j.maxTotal > j.length + j.latency &&
               !(j.tailEnd >= 0 && j.tailEnd <= j.maxTotal);
    j.rendered = pos;
}

}  // namespace

Result<RenderResult> render(std::span<host::Instance* const> chain, const RenderSettings& settings,
                            const RenderRequest& request) {
    if (chain.empty())
        return Error("render.empty_chain", "nothing to render through", {}, "load a plugin first");
    if (!(settings.sampleRate >= 8000 && settings.sampleRate <= 384000))
        return Error("render.sample_rate", "sample rate out of range", {}, "use 8000 .. 384000 Hz");
    if (settings.blockSize < 1 || settings.blockSize > 8192)
        return Error("render.block_size", "block size out of range", {}, "use 1 .. 8192 frames");
    if (!(settings.tempo >= 1 && settings.tempo <= 999) || settings.numerator < 1 ||
        settings.numerator > 64 || settings.denominator < 1 || settings.denominator > 64 ||
        (settings.denominator & (settings.denominator - 1)))
        return Error("render.transport", "tempo or time signature out of range", {},
                     "use tempo 1 .. 999 and a valid time signature");
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (!chain[i] || chain[i]->processing() ||
            std::find(chain.begin(), chain.begin() + static_cast<std::ptrdiff_t>(i), chain[i]) !=
                chain.begin() + static_cast<std::ptrdiff_t>(i))
            return Error("render.chain", "chain contains a null, processing or repeated instance");
        host::Instance& in = *chain[i];
        if (in.audioOutputChannels() != 2)
            return Error("render.layout", in.info().name + " does not have a stereo output", {},
                         "the sandbox renders stereo chains");
        if (i > 0 && in.audioInputChannels() != 2)
            return Error("render.layout",
                         in.info().name + " cannot follow another plugin (no stereo input)", {},
                         "instruments go first in the chain");
    }
    if (!(settings.tailLimitSeconds >= 0 && settings.tailLimitSeconds <= 600))
        return Error("render.tail", "tail limit out of range", {}, "0 .. 600 seconds");
    if (!(settings.silenceHoldSeconds > 0 && settings.silenceHoldSeconds <= 60) ||
        !(settings.silenceDb <= 0 && settings.silenceDb >= -400))
        return Error("render.silence", "silence detection settings out of range", {},
                     "hold (0, 60] s, threshold -400 .. 0 dB");
    if (request.lengthSamples < 0 || request.lengthSamples > kMaxRenderFrames)
        return Error("render.too_long", "render length out of range", {},
                     "at most " + std::to_string(kMaxRenderFrames) + " frames");
    for (std::size_t i = 0; i < request.events.size(); ++i) {
        const auto at = request.events[i].sample;
        if (at < 0 || at > kMaxRenderFrames)
            return Error("render.event_time",
                         "event " + std::to_string(i) + " is at sample " + std::to_string(at), {},
                         "events must lie in 0 .. " + std::to_string(kMaxRenderFrames));
    }
    if (request.input && request.input->frames() > static_cast<std::size_t>(kMaxRenderFrames))
        return Error("render.too_long", "input audio is too long", {},
                     "at most " + std::to_string(kMaxRenderFrames) + " frames");
    if (request.input && request.input->sampleRate != settings.sampleRate)
        return Error("render.input_rate",
                     "input audio is at " + std::to_string(request.input->sampleRate) + " Hz", {},
                     "resample it to the session rate first");
    if (request.input)
        for (const auto& channel : request.input->channels)
            if (channel.size() != request.input->frames())
                return Error("render.input_layout", "input channels have different lengths");

    // Main-thread preparation: (re)activate, service pending restarts.
    for (host::Instance* in : chain) {
        in->serviceMainThread();
        if (in->active() &&
            (in->sampleRate() != settings.sampleRate || in->maxFrames() < settings.blockSize))
            in->deactivate();
        if (!in->active()) SUPPLE_TRY(in->activate(settings.sampleRate, 1, settings.blockSize));
    }

    Job j;
    j.settings = &settings;
    j.request = &request;
    j.events = request.events;
    std::stable_sort(j.events.begin(), j.events.end(),
                     [](const TimedEvent& a, const TimedEvent& b) { return a.sample < b.sample; });
    for (host::Instance* in : chain) j.latency += in->latency();
    j.length = request.lengthSamples > 0
                   ? request.lengthSamples
                   : (request.input ? static_cast<std::int64_t>(request.input->frames()) : 0);
    // An explicit length is authoritative (events past it land in the tail);
    // otherwise the render covers the input and every event.
    if (!j.events.empty() && request.lengthSamples <= 0)
        j.length = std::max(j.length, j.events.back().sample + 1);
    const auto tailMax =
        static_cast<std::int64_t>(std::llround(settings.tailLimitSeconds * settings.sampleRate));
    j.maxTotal = j.length + j.latency + std::max<std::int64_t>(0, tailMax);
    if (j.maxTotal > kMaxRenderFrames)
        return Error("render.too_long",
                     "the render would need " + std::to_string(j.maxTotal) + " frames", {},
                     "shorten the input or the tail limit (at most " +
                         std::to_string(kMaxRenderFrames) + " frames)");
    // When every stage reports its tail (clap.tail), the render may end when
    // the chain's total tail has elapsed; an infinite or unreported tail
    // leaves it to silence detection and the cap.
    j.tailsReported = std::all_of(chain.begin(), chain.end(),
                                  [](host::Instance* in) { return in->reportsTail(); });
    if (j.tailsReported) {
        std::int64_t tail = 0;
        for (host::Instance* in : chain) {
            if (in->tail() == UINT32_MAX) {
                tail = -1;
                break;
            }
            tail += in->tail();
        }
        if (tail >= 0) j.tailEnd = j.length + j.latency + tail;
    }
    j.silenceHold =
        std::max<std::int64_t>(1, std::llround(settings.silenceHoldSeconds * settings.sampleRate));
    j.silenceLinear = std::pow(10.0, settings.silenceDb / 20.0);
    const std::size_t block = settings.blockSize;
    j.zeroL.assign(block, 0.0f);
    j.zeroR.assign(block, 0.0f);
    j.inL.assign(block, 0.0f);
    j.inR.assign(block, 0.0f);
    j.outL.assign(static_cast<std::size_t>(j.maxTotal), 0.0f);
    j.outR.assign(static_cast<std::size_t>(j.maxTotal), 0.0f);
    j.stages.resize(chain.size());
    std::vector<std::size_t> perTarget(chain.size(), 0);
    for (const auto& e : j.events)
        if (e.target < chain.size()) ++perTarget[e.target];
    for (std::size_t i = 0; i < chain.size(); ++i) {
        Stage& st = j.stages[i];
        st.instance = chain[i];
        st.left.assign(block, 0.0f);
        st.right.assign(block, 0.0f);
        st.out[0] = st.left.data();
        st.out[1] = st.right.data();
        st.blockEvents.reserve(perTarget[i] + 1);
        st.outStorage.resize(512);
        st.outEvents.storage = st.outStorage;
    }

    std::thread audio([&] {
        supple::nameThread("audio-offline");
        bool started = true;
        for (host::Instance* in : chain) {
            if (settings.resetBeforeRender) in->reset();
            in->flushParams();
            started = in->startProcessing() && started;
        }
        if (started)
            renderLoop(j);
        else
            j.startFailed = true;
        for (host::Instance* in : chain) in->stopProcessing();
    });
    audio.join();

    RenderResult r;
    for (host::Instance* in : chain) {
        in->serviceMainThread();
        for (auto& line : in->takeLog()) r.log.push_back(in->info().name + ": " + line);
    }
    if (j.startFailed)
        return Error("render.start_failed", "a plugin refused to start processing", {},
                     "check the plugin log (render result log)");
    if (j.error)
        return Error("render.process_error",
                     "a plugin reported a processing error at sample " + std::to_string(j.rendered),
                     {}, "check the plugin log (render result log) and its settings");
    r.latency = static_cast<std::uint32_t>(j.latency);
    r.tailCapped = j.capped;
    const std::int64_t keep = std::max<std::int64_t>(0, j.rendered - j.latency);
    r.tailSamples = std::max<std::int64_t>(0, keep - j.length);
    r.audio.sampleRate = settings.sampleRate;
    r.audio.channels.resize(2);
    const auto begin = static_cast<std::ptrdiff_t>(j.latency);
    r.audio.channels[0].assign(j.outL.begin() + begin, j.outL.begin() + begin + keep);
    r.audio.channels[1].assign(j.outR.begin() + begin, j.outR.begin() + begin + keep);
    float peak = 0.0f;
    for (const auto& c : r.audio.channels)
        for (float v : c) peak = std::max(peak, std::fabs(v));
    r.peakDbfs = peak > 0 ? 20.0 * std::log10(static_cast<double>(peak)) : -1000.0;
    return r;
}

}  // namespace supple::sandbox
