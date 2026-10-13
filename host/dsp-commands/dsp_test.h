// dsp.test, the DSP script harness (ADR-0032), shared by the engine and the
// sandbox: standard signals through a fresh instance of an effect, or notes
// into an instrument (a script's, through the script player, or any other),
// each measured, with what looks wrong said in words. It renders with the sandbox's offline
// renderer (supple/sandbox/render.h), the same in both.
//
// Host is the Context's service and has findPlugin(id), plugins(rescan) and
// scriptDir(); TestTraits<Host> gives the render settings, the host identity
// and the folder for saved outputs.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "dsp_shared.h"
#include "supple/audio/audio_file.h"
#include "supple/clap/semantic_params.h"
#include "supple/dsp/dsp.h"
#include "supple/host/clap_host.h"
#include "supple/sandbox/render.h"

namespace supple::dsp_commands {

template <class Host>
struct TestTraits;  // static settings(h), identity(h), outputDir(h)

namespace test {
namespace fs = std::filesystem;

enum class Signal { Sine, Sweep, Impulse, Noise, Silence, Notes, Note };
inline constexpr Signal kEffectSignals[] = {Signal::Sine, Signal::Sweep, Signal::Impulse,
                                            Signal::Noise, Signal::Silence};
inline constexpr Signal kInstrumentSignals[] = {Signal::Notes, Signal::Note, Signal::Silence};
inline bool forInstruments(Signal s) { return s == Signal::Notes || s == Signal::Note; }

inline const char* signalName(Signal s) {
    switch (s) {
        case Signal::Sine:
            return "sine";
        case Signal::Sweep:
            return "sweep";
        case Signal::Impulse:
            return "impulse";
        case Signal::Noise:
            return "noise";
        case Signal::Notes:
            return "notes";
        case Signal::Note:
            return "note";
        default:
            return "silence";
    }
}

struct TestArgs {
    std::optional<std::string> plugin, script;
    std::optional<json::Value> patch;
    std::optional<std::vector<Signal>> signals;
    double levelDb = -12.0;
    double frequencyHz = 1000.0;
    double seconds = 1.0;
    bool save = false;
};
struct ResponsePoint {
    double hz = 0.0, db = 0.0;
};
struct SignalResult {
    std::string signal;
    double peakDbfs = -200.0, rmsDbfs = -200.0, dc = 0.0;
    std::int64_t clipped = 0, denormals = 0;
    std::optional<double> gainDb, thdPercent;
    std::optional<std::vector<ResponsePoint>> response;
    std::optional<double> pitchHz, releaseMs;
    std::optional<std::string> path;
};
struct TestResult {
    std::string plugin;
    std::int64_t latency = 0;
    std::vector<SignalResult> signals;
    double cpuPercent = 0.0;
    std::vector<std::string> problems;
    std::vector<std::string> log;
};

inline double toDb(double amplitude) {
    return amplitude > 1e-10 ? 20.0 * std::log10(amplitude) : -200.0;
}
// What a reader needs: dB to a hundredth, the rest to a few digits. Divided
// by a power of ten, so the result is the double nearest the decimal.
inline double rounded(double v, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return std::round(v * scale) / scale;
}

// Stereo, the same on both channels, at the session's rate.
inline audio::AudioData stimulus(Signal s, double rate, double seconds, double levelDb, double hz) {
    const auto frames = static_cast<std::size_t>(std::llround(rate * seconds));
    audio::AudioData a;
    a.sampleRate = rate;
    std::vector<float> x(frames, 0.0f);
    const double amp = std::pow(10.0, levelDb / 20.0);
    const double twoPi = 2.0 * std::numbers::pi;
    switch (s) {
        case Signal::Sine:
            for (std::size_t i = 0; i < frames; ++i)
                x[i] =
                    static_cast<float>(amp * std::sin(twoPi * hz * static_cast<double>(i) / rate));
            break;
        case Signal::Sweep: {
            // Exponential, 20 Hz to 20 kHz (or 0.45 of the rate).
            const double f0 = 20.0, f1 = std::min(20000.0, 0.45 * rate);
            const double k = std::log(f1 / f0);
            for (std::size_t i = 0; i < frames; ++i) {
                const double t = static_cast<double>(i) / rate;
                const double phase = twoPi * f0 * seconds / k * (std::exp(t / seconds * k) - 1.0);
                x[i] = static_cast<float>(amp * std::sin(phase));
            }
            break;
        }
        case Signal::Impulse:
            if (frames > 0) x[0] = static_cast<float>(amp);
            break;
        case Signal::Noise: {
            // Pink (Paul Kellet's filter on seeded white noise), without DC,
            // peaking at the level like every other signal.
            std::mt19937 rng(0x5eed1234u);
            std::uniform_real_distribution<double> white(-1.0, 1.0);
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;
            std::vector<double> y(frames);
            for (std::size_t i = 0; i < frames; ++i) {
                const double w = white(rng);
                b0 = 0.99886 * b0 + w * 0.0555179;
                b1 = 0.99332 * b1 + w * 0.0750759;
                b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856;
                b4 = 0.55000 * b4 + w * 0.5329522;
                b5 = -0.7616 * b5 - w * 0.0168980;
                y[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
                b6 = w * 0.115926;
                sum += y[i];
            }
            const double mean = frames ? sum / static_cast<double>(frames) : 0.0;
            double peak = 0.0;
            for (double& v : y) peak = std::max(peak, std::abs(v -= mean));
            const double scale = peak > 0.0 ? amp / peak : 0.0;
            for (std::size_t i = 0; i < frames; ++i) x[i] = static_cast<float>(y[i] * scale);
            break;
        }
        case Signal::Silence:
        case Signal::Notes:  // an instrument's: notes, no audio
        case Signal::Note:
            break;
    }
    a.channels.assign(2, x);
    return a;
}

// An instrument's stimulus: its notes (key, on, at a fraction of `seconds`),
// at velocity 0.8. notes: a note, then a chord, released 0.2 s before the
// end; note: A4 (key 69), released likewise; silence: none.
inline std::vector<sandbox::TimedEvent> noteEvents(Signal s, double rate, double seconds) {
    std::vector<sandbox::TimedEvent> out;
    auto add = [&](double at, int key, bool on) {
        sandbox::TimedEvent e;
        e.sample = static_cast<std::int64_t>(std::llround(at * seconds * rate));
        e.event.kind = on ? host::Event::Kind::NoteOn : host::Event::Kind::NoteOff;
        e.event.key = static_cast<std::int16_t>(key);
        e.event.velocity = 0.8;
        out.push_back(e);
    };
    if (s == Signal::Notes) {
        add(0.0, 60, true);
        add(0.2, 60, false);
        for (int key : {60, 64, 67}) add(0.3, key, true);
        for (int key : {60, 64, 67}) add(0.8, key, false);
    } else if (s == Signal::Note) {
        add(0.0, 69, true);
        add(0.8, 69, false);
    }
    return out;
}
// When the last note is released, as a fraction of `seconds`.
inline constexpr double kReleaseAt = 0.8;

// The fundamental of x over 4096 samples from `begin` (the difference
// function's first dip, as YIN finds it), between 40 Hz and 2 kHz, and how
// periodic the sound is there (1: exactly); nothing when it is not a tone.
inline std::optional<std::pair<double, double>> pitchOf(const std::vector<float>& x,
                                                        std::size_t begin, double rate) {
    constexpr std::size_t kWindow = 4096;
    const auto minLag = static_cast<std::size_t>(rate / 2000.0);
    const auto maxLag = static_cast<std::size_t>(rate / 40.0);
    if (begin + kWindow + maxLag + 2 > x.size()) return std::nullopt;
    std::vector<double> d(maxLag + 2, 0.0);
    double running = 0.0;
    d[0] = 1.0;
    for (std::size_t tau = 1; tau <= maxLag + 1; ++tau) {
        double sum = 0.0;
        for (std::size_t i = begin; i < begin + kWindow; ++i) {
            const double diff = static_cast<double>(x[i]) - static_cast<double>(x[i + tau]);
            sum += diff * diff;
        }
        running += sum;
        d[tau] = running > 0.0 ? sum * static_cast<double>(tau) / running : 1.0;
    }
    for (std::size_t tau = std::max<std::size_t>(minLag, 2); tau <= maxLag; ++tau) {
        if (d[tau] > 0.15) continue;
        while (tau + 1 <= maxLag && d[tau + 1] < d[tau]) ++tau;  // to the dip's bottom
        const double a = d[tau - 1], b = d[tau], c = d[tau + 1];
        const double denom = a - 2.0 * b + c;
        const double shift = denom > 0.0 ? 0.5 * (a - c) / denom : 0.0;
        return std::make_pair(rate / (static_cast<double>(tau) + shift), 1.0 - b);
    }
    return std::nullopt;
}

// The amplitude of a tone at hz in x[begin, end): a Hann-windowed DFT bin.
inline double toneAmplitude(const std::vector<float>& x, std::size_t begin, std::size_t end,
                            double hz, double rate) {
    if (end <= begin + 1) return 0.0;
    const double n = static_cast<double>(end - begin);
    std::complex<double> acc = 0.0;
    double weights = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        const double t = static_cast<double>(i - begin);
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * t / (n - 1.0));
        acc += w * static_cast<double>(x[i]) *
               std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
        weights += w;
    }
    return 2.0 * std::abs(acc) / weights;
}

// Levels and counts over every channel; gain, THD and response by signal.
inline SignalResult measure(Signal s, const audio::AudioData& out, std::size_t inputFrames,
                            const TestArgs& a) {
    SignalResult r;
    r.signal = signalName(s);
    double peak = 0.0, squares = 0.0;
    std::size_t count = 0;
    for (const auto& ch : out.channels) {
        double sum = 0.0;
        for (const float v : ch) {
            const double d = static_cast<double>(v);
            peak = std::max(peak, std::abs(d));
            squares += d * d;
            sum += d;
            if (std::abs(d) > 1.0) ++r.clipped;
            if (std::fpclassify(v) == FP_SUBNORMAL) ++r.denormals;
        }
        count += ch.size();
        if (!ch.empty()) r.dc = std::max(r.dc, std::abs(sum / static_cast<double>(ch.size())));
    }
    r.peakDbfs = rounded(toDb(peak), 2);
    r.rmsDbfs = rounded(toDb(count ? std::sqrt(squares / static_cast<double>(count)) : 0.0), 2);
    r.dc = rounded(r.dc, 6);
    if (out.channels.empty()) return r;
    const std::vector<float>& x = out.channels[0];
    const double rate = out.sampleRate;
    const double amp = std::pow(10.0, a.levelDb / 20.0);
    if (s == Signal::Note) {
        // Past the attack, before any envelope has had long to fall.
        if (auto pitch = pitchOf(x, static_cast<std::size_t>(0.03 * rate), rate))
            r.pitchHz = rounded(pitch->first, 2);
    }
    if (forInstruments(s)) {
        // From the last release to the last sample above -80 dBFS.
        const auto released = static_cast<std::size_t>(kReleaseAt * a.seconds * rate);
        std::size_t last = 0;
        for (const auto& ch : out.channels)
            for (std::size_t i = ch.size(); i-- > 0;)
                if (std::abs(ch[i]) > 1e-4f) {
                    last = std::max(last, i + 1);
                    break;
                }
        r.releaseMs = rounded(
            last > released ? 1000.0 * static_cast<double>(last - released) / rate : 0.0, 1);
    }
    if (s == Signal::Sine) {
        // The steady part: past the first 100 ms, within the input.
        const std::size_t end = std::min(inputFrames, x.size());
        const std::size_t begin = std::min(static_cast<std::size_t>(0.1 * rate), end / 4);
        const double fundamental = toneAmplitude(x, begin, end, a.frequencyHz, rate);
        r.gainDb = toDb(fundamental) - toDb(amp);
        if (fundamental > 1e-7) {
            double harmonics = 0.0;
            for (int k = 2; k <= 10 && k * a.frequencyHz < 0.45 * rate; ++k) {
                const double h = toneAmplitude(x, begin, end, k * a.frequencyHz, rate);
                harmonics += h * h;
            }
            r.thdPercent = rounded(100.0 * std::sqrt(harmonics) / fundamental, 4);
        }
        r.gainDb = rounded(*r.gainDb, 2);
    } else if (s == Signal::Impulse) {
        std::size_t n = 1;
        while (n < x.size() && n < (std::size_t(1) << 19)) n <<= 1;
        std::vector<double> in(n, 0.0);
        for (std::size_t i = 0; i < std::min(n, x.size()); ++i) in[i] = static_cast<double>(x[i]);
        std::vector<std::complex<double>> spectrum(n / 2 + 1);
        dsp::Fft fft;
        fft.prepare(n);
        if (fft.forward(in, spectrum)) {
            r.response.emplace();
            for (double hz = 31.25; hz < 0.45 * rate && hz <= 16000.0; hz *= 2.0) {
                const auto bin =
                    static_cast<std::size_t>(std::llround(hz * static_cast<double>(n) / rate));
                if (bin < spectrum.size())
                    r.response->push_back({hz, rounded(toDb(std::abs(spectrum[bin]) / amp), 2)});
            }
        }
    }
    return r;
}

inline std::string fixed(double v, int digits = 1) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", digits, v);
    return text;
}

}  // namespace test

template <class Host>
void registerDspTest(cmd::Registry& registry, const std::string& exampleEffect) {
    using namespace test;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<TestArgs, TestResult> c;
    c.name = "dsp.test";
    c.kind = Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Play test signals or notes through a plugin and measure it: levels, THD, pitch.";
    c.description =
        "Give plugin (an id: a script's, or any effect's or instrument's) or script (a name in "
        "the library). A fresh instance, set by patch (a merge patch) when given, plays each "
        "signal. An effect: sine (frequency_hz; gain_db and thd_percent), sweep (20 Hz to 20 "
        "kHz), impulse (response_db at octave centres), pink noise and silence, each peaking at "
        "level_db. An instrument, at velocity 0.8: notes (a note, then a chord), note (A4; "
        "pitch_hz, the pitch heard), and silence (no notes); release_ms is how long the sound "
        "lasts after the last release. Each reports peak and RMS in dBFS, DC, samples beyond 0 "
        "dBFS and denormals; problems says in words what looks wrong (clipping, DC, sound from "
        "silence, no output, a note out of tune, sound that never dies away, non-finite "
        "samples the framework silenced, a script stopped by its loop budget or indexing "
        "outside an array). cpu_percent is measured, so it varies between runs. save: true "
        "keeps each output as a WAV (path) for analyze and compare.";
    c.tags = {"dsp", "analysis"};
    c.args = schema::object<TestArgs>("dsp_test_args")
                 .field("plugin", &TestArgs::plugin, schema::string().minLength(1))
                 .field("script", &TestArgs::script, schema::string().minLength(1))
                 .field("patch", &TestArgs::patch, schema::any())
                 .field("signals", &TestArgs::signals,
                        schema::arrayOf(schema::enumeration<Signal>({{Signal::Sine, "sine"},
                                                                     {Signal::Sweep, "sweep"},
                                                                     {Signal::Impulse, "impulse"},
                                                                     {Signal::Noise, "noise"},
                                                                     {Signal::Silence, "silence"},
                                                                     {Signal::Notes, "notes"},
                                                                     {Signal::Note, "note"}}))
                            .doc("default: an effect's five, an instrument's three"))
                 .field("level_db", &TestArgs::levelDb,
                        schema::number().range(-60, 0).unit(units::Unit::dB))
                 .defaultValue(-12.0)
                 .field("frequency_hz", &TestArgs::frequencyHz,
                        schema::number().range(10, 20000).unit(units::Unit::Hz))
                 .defaultValue(1000.0)
                 .field("seconds", &TestArgs::seconds, schema::number().range(0.1, 10))
                 .defaultValue(1.0)
                 .field("save", &TestArgs::save, schema::boolean())
                 .defaultValue(false)
                 .build();
    auto point = schema::object<ResponsePoint>("dsp_response_point")
                     .field("hz", &ResponsePoint::hz, schema::number())
                     .field("db", &ResponsePoint::db, schema::number())
                     .build();
    auto signal = schema::object<SignalResult>("dsp_signal_result")
                      .field("signal", &SignalResult::signal, schema::string())
                      .field("peak_dbfs", &SignalResult::peakDbfs, schema::number())
                      .field("rms_dbfs", &SignalResult::rmsDbfs, schema::number())
                      .field("dc", &SignalResult::dc, schema::number())
                      .field("clipped", &SignalResult::clipped, schema::integer().min(0))
                      .field("denormals", &SignalResult::denormals, schema::integer().min(0))
                      .field("gain_db", &SignalResult::gainDb,
                             schema::number().doc("sine: the fundamental out against in"))
                      .field("thd_percent", &SignalResult::thdPercent, schema::number())
                      .field("response_db", &SignalResult::response, schema::arrayOf(point))
                      .field("pitch_hz", &SignalResult::pitchHz,
                             schema::number().doc("note: the pitch heard (A4 is 440 Hz)"))
                      .field("release_ms", &SignalResult::releaseMs,
                             schema::number().doc("an instrument's: sound after the last release"))
                      .field("path", &SignalResult::path, schema::string())
                      .build();
    c.result = schema::object<TestResult>("dsp_test_result")
                   .field("plugin", &TestResult::plugin, schema::string())
                   .field("latency", &TestResult::latency, schema::integer().doc("samples"))
                   .field("signals", &TestResult::signals, schema::arrayOf(signal))
                   .field("cpu_percent", &TestResult::cpuPercent,
                          schema::number().doc("of real time, measured"))
                   .field("problems", &TestResult::problems, schema::arrayOf(schema::string()))
                   .field("log", &TestResult::log, schema::arrayOf(schema::string()))
                   .build();
    c.errors = {{"dsp.args", "neither or both of plugin and script", "give exactly one"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.not_effect", "the plugin takes no audio and no notes",
                 "test an effect or an instrument"},
                {"dsp.signal", "a signal the plugin cannot play",
                 "an effect plays audio signals, an instrument notes"}};
    if (!exampleEffect.empty()) {
        json::Value args = json::Value::object();
        args["plugin"] = exampleEffect;
        args["signals"] = json::Value::array();
        args["signals"].push_back("sine");
        c.examples = {{"Measure an effect with a sine", args,
                       detail::j(R"({"plugin":"<any>","latency":"<any>",
                          "signals":"<any>","cpu_percent":"<any>","problems":"<any>","log":"<any>"})"),
                       "empty"}};
    }
    c.run = [](const TestArgs& a, Context& ctx) -> Result<TestResult> {
        Host& s = ctx.service<Host>();
        using Traits = TestTraits<Host>;
        if (a.plugin.has_value() == a.script.has_value())
            return Error("dsp.args", "give exactly one of plugin and script", "",
                         "plugin is an id from plugin.list; script is a name from dsp.list");
        std::string id;
        if (a.plugin) {
            id = *a.plugin;
        } else {
            auto text = detail::readScript(s, *a.script, "/script");
            if (!text) return std::move(text).error();
            auto compiled = detail::compile(*text);
            if (!compiled.plugin)
                return Error("dsp.compile", "the script does not compile", "/script",
                             "dsp.check {name: \"" + *a.script + "\"} gives its diagnostics");
            id = compiled.plugin->id;
        }
        auto info = s.findPlugin(id);
        if (!info && a.script) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
            if (!info)
                return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                             "the script player must be on the sandbox's plugin path "
                             "(--plugin-path)");
        }
        if (!info) return std::move(info).error().at("/plugin");
        auto created = host::Instance::create(*info, Traits::identity(s));
        if (!created) return std::move(created).error();
        host::Instance& instance = **created;
        const bool instrument = instance.audioInputChannels() == 0;
        if (instrument && !info->instrument)
            return Error("dsp.not_effect", info->name + " takes no audio and no notes",
                         a.plugin ? "/plugin" : "/script",
                         "dsp.test measures effects and instruments");
        if (a.patch) {
            if (!instance.info().semantic)
                return Error("dsp.not_semantic", info->name + " takes no patch", "/patch",
                             "set it by plugin.load and param.set instead");
            SUPPLE_TRY(instance.setPatch(json::writeCompact(*a.patch), CLAP_SUPPLE_SET_MERGE));
        }

        TestResult out;
        out.plugin = info->id;
        const std::vector<Signal> signals =
            a.signals && !a.signals->empty() ? *a.signals
            : instrument
                ? std::vector<Signal>(std::begin(kInstrumentSignals), std::end(kInstrumentSignals))
                : std::vector<Signal>(std::begin(kEffectSignals), std::end(kEffectSignals));
        for (size_t k = 0; k < signals.size(); ++k)
            if (signals[k] != Signal::Silence && forInstruments(signals[k]) != instrument)
                return Error("dsp.signal",
                             info->name +
                                 (instrument ? " is an instrument: it plays notes, not "
                                             : " is an effect: it takes audio, not ") +
                                 signalName(signals[k]),
                             "/signals/" + std::to_string(k),
                             instrument ? "use notes, note or silence"
                                        : "use sine, sweep, impulse, noise or silence");
        std::set<std::string> logged;
        double rendered = 0.0, spent = 0.0;
        host::Instance* chain[] = {&instance};
        const sandbox::RenderSettings settings = Traits::settings(s);
        for (const Signal sig : signals) {
            const audio::AudioData input =
                instrument
                    ? audio::AudioData{}
                    : stimulus(sig, settings.sampleRate, a.seconds, a.levelDb, a.frequencyHz);
            const std::size_t inputFrames =
                instrument ? static_cast<std::size_t>(std::llround(a.seconds * settings.sampleRate))
                           : input.frames();
            sandbox::RenderRequest request;
            if (instrument) {
                request.events = noteEvents(sig, settings.sampleRate, a.seconds);
                request.lengthSamples = static_cast<std::int64_t>(inputFrames);
            } else {
                request.input = &input;
            }
            const auto start = std::chrono::steady_clock::now();
            auto r = sandbox::render(chain, settings, request);
            spent +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (!r) return std::move(r).error();
            rendered += r->audio.seconds();
            out.latency = r->latency;
            for (const std::string& line : r->log)
                if (logged.insert(line).second) out.log.push_back(line);
            SignalResult m = measure(sig, r->audio, inputFrames, a);
            if (a.save) {
                const fs::path file =
                    Traits::outputDir(s) / ("dsp-test-" + info->id + "-" + m.signal + ".wav");
                SUPPLE_TRY(audio::writeWav(file, r->audio));
                m.path = file.string();
            }
            const std::string with = " with the " + m.signal;
            if (m.clipped > 0)
                out.problems.push_back(std::to_string(m.clipped) + " samples beyond 0 dBFS" + with +
                                       " (peak " + fixed(m.peakDbfs) + " dBFS)");
            if (m.dc > 0.01) out.problems.push_back("a DC offset of " + fixed(m.dc, 3) + with);
            if (sig == Signal::Silence && m.peakDbfs > -100.0)
                out.problems.push_back(
                    (instrument ? "sound with no notes: peak " : "sound from silence: peak ") +
                    fixed(m.peakDbfs) +
                    (instrument ? " dBFS (a voice that plays without a note, or a source that "
                                  "runs free)"
                                : " dBFS (feedback that sustains itself, or a source that runs "
                                  "free)"));
            if (sig != Signal::Silence && m.peakDbfs < -120.0)
                out.problems.push_back(
                    instrument ? "no sound" + with +
                                     " (does startVoice start the voice, and process() add "
                                     "each voice that voicePlaying(v) into io?)"
                               : "no output" + with);
            if (instrument && sig != Signal::Silence && r->tailCapped)
                out.problems.push_back(
                    "still sounding " + fixed(settings.tailLimitSeconds) +
                    " s after every note was released" + with +
                    " (does releaseVoice start each voice's release, and process() call "
                    "voiceFinished(v) once it has died away?)");
            if (sig == Signal::Note && m.pitchHz) {
                const double cents = 1200.0 * std::log2(*m.pitchHz / 440.0);
                if (std::abs(cents) > 50.0)
                    out.problems.push_back("A4 (key 69) sounded at " + fixed(*m.pitchHz) + " Hz, " +
                                           fixed(cents, 0) +
                                           " cents from 440 Hz (voiceHz(v, i) is the note's "
                                           "frequency)");
            }
            out.signals.push_back(std::move(m));
        }
        out.cpuPercent = rendered > 0.0 ? rounded(100.0 * spent / rendered, 2) : 0.0;
        for (const std::string& line : out.log) {
            if (line.find("non-finite") != std::string::npos)
                out.problems.push_back(
                    "it produced NaN or infinity, which the framework silenced: "
                    "look for a division by zero, log of 0 or growing feedback");
            else if (line.find("loop budget") != std::string::npos)
                out.problems.push_back("a loop never ended: " + line);
            else if (line.find("outside an array") != std::string::npos)
                out.problems.push_back("an index went out of range: " + line);
        }
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
