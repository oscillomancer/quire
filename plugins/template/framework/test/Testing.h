// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Helpers for testing DSP the way a host runs it: through the same Engine the
// plugin uses, with host block sizes and sample-accurate automation chosen by
// the test.
//
//   test::RenderOptions o;
//   o.blocks = {1, 7, 64, 513};
//   o.initial = {{kCutoff, 1000.0}};
//   o.automation = {{4800, kCutoff, 200.0}};  // plain units, absolute frame
//   const test::Signal out = test::render(test::sine(2, 48000, 100.0, 48000.0), o);
//
// Products with more than one bus or with notes use the same call:
// `o.sidechain` feeds the sidechain, `o.aux` collects the aux outputs,
// `o.events` delivers notes and MIDI (test::noteOn, test::midi, ...) at their
// frames, and `o.sent` collects what the processor sent (note ends, notes,
// MIDI). An instrument's input is ignored; a note effect renders o.frames.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <string>
#include <vector>

#include "Check.h"
#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/PropertyTable.h"

namespace supple::test {

using Signal = std::vector<std::vector<double>>;  // [channel][frame]

inline Signal silence(int channels, int frames) {
    return Signal(static_cast<size_t>(channels), std::vector<double>(static_cast<size_t>(frames), 0.0));
}

inline Signal sine(int channels, int frames, double hz, double sampleRate, double amplitude = 0.5) {
    Signal s = silence(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            s[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                amplitude * std::sin(2.0 * dsp::kPi * hz * i / sampleRate + 0.37 * c);
    return s;
}

inline Signal noise(int channels, int frames, uint32_t seed = 1, double amplitude = 0.5) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> d(-amplitude, amplitude);
    Signal s = silence(channels, frames);
    for (auto& channel : s)
        for (auto& x : channel) x = d(rng);
    return s;
}

inline double peak(const Signal& s, int from = 0) {
    double p = 0.0;
    for (const auto& channel : s)
        for (size_t i = static_cast<size_t>(from); i < channel.size(); ++i) p = std::max(p, std::fabs(channel[i]));
    return p;
}

inline double rms(const Signal& s, int from = 0) {
    double sum = 0.0;
    size_t n = 0;
    for (const auto& channel : s)
        for (size_t i = static_cast<size_t>(from); i < channel.size(); ++i) {
            sum += channel[i] * channel[i];
            ++n;
        }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

inline double rmsDb(const Signal& s, int from = 0) { return dsp::gainToDb(rms(s, from), -200.0); }

inline double maxDifference(const Signal& a, const Signal& b) {
    if (a.size() != b.size()) return INFINITY;
    double d = 0.0;
    for (size_t c = 0; c < a.size(); ++c) {
        if (a[c].size() != b[c].size()) return INFINITY;
        for (size_t i = 0; i < a[c].size(); ++i) d = std::max(d, finiteDifference(a[c][i], b[c][i]));
    }
    return d;
}

inline bool allFinite(const Signal& s) {
    for (const auto& channel : s)
        for (double x : channel)
            if (!std::isfinite(x)) return false;
    return true;
}

// A parameter change in plain units at an absolute frame.
struct Automation {
    int frame;
    uint32_t id;
    double plain;
};

// Any other input event (a note, MIDI, an expression) at an absolute frame.
struct EventAt {
    int frame;
    EngineEvent event;
};

inline EventAt noteOn(int frame, int key, double velocity = 1.0, int noteId = -1, int channel = 0) {
    EventAt e{frame, {}};
    e.event.type = EngineEvent::Type::Note;
    e.event.note.type = NoteEvent::Type::On;
    e.event.note.key = static_cast<int16_t>(key);
    e.event.note.channel = static_cast<int16_t>(channel);
    e.event.note.noteId = noteId;
    e.event.note.velocity = velocity;
    return e;
}
inline EventAt noteOff(int frame, int key, int noteId = -1, int channel = 0) {
    EventAt e = noteOn(frame, key, 0.0, noteId, channel);
    e.event.note.type = NoteEvent::Type::Off;
    return e;
}
inline EventAt midi(int frame, const MidiMessage& m) {
    EventAt e{frame, {}};
    e.event.type = EngineEvent::Type::Midi;
    e.event.midi = m;
    return e;
}
inline EventAt expression(int frame, NoteExpression::Type type, double value, NoteAddress address = {}) {
    EventAt e{frame, {}};
    e.event.type = EngineEvent::Type::Expression;
    e.event.expression = type;
    e.event.value = value;
    e.event.address = address;
    return e;
}
// Per-note modulation of a polyModulatable() parameter, in host units.
inline EventAt noteModulation(int frame, uint32_t id, double amount, NoteAddress address) {
    EventAt e{frame, {}};
    e.event.type = EngineEvent::Type::NoteModulation;
    e.event.index = params::indexOf(id);
    e.event.value = amount;
    e.event.address = address;
    return e;
}

// What the processor sent, at an absolute frame.
struct Sent {
    int frame;
    NoteOutput::Kind kind;
    NoteEvent note;
    MidiMessage midi;
};

struct RenderOptions {
    double sampleRate = 48000.0;
    std::vector<int> blocks = {512};               // host block sizes, cycled
    std::vector<std::pair<uint32_t, double>> initial;  // plain values before activation
    std::vector<std::pair<uint32_t, std::string>> properties;  // property values before activation
    std::vector<Automation> automation;            // any order; applied at their frames
    bool metering = false;
    Signal sidechain;                  // PRODUCT_SIDECHAIN: the key; empty leaves it unconnected
    std::vector<Signal>* aux = nullptr;  // PRODUCT_AUX_OUTPUTS: receives each aux output
    std::vector<EventAt> events;       // notes, MIDI, expressions; any order
    std::vector<Sent>* sent = nullptr;   // receives what the processor sent
    int frames = 0;                    // a note effect's length (it has no input to measure)
    bool activate = true;              // false: carry on with an engine already active
    bool offline = false;              // render as a host does a bounce (Processor::kOfflineMode)
    TuningSource* tuning = nullptr;    // PRODUCT_TUNING: where retuning() comes from
};

// Renders an effect's input (an instrument's or note effect's length)
// through a fresh Engine, splitting it into the given host blocks and
// delivering automation and events at their exact frames. Pass an Engine to
// inspect it afterwards (telemetry, safety resets).
inline Signal render(const Signal& input, const RenderOptions& o, Engine* engine = nullptr) {
    std::unique_ptr<Engine> owned;
    if (!engine) {
        owned = std::make_unique<Engine>();
        engine = owned.get();
    }
    const int channels = product::info::kAudioOutput ? static_cast<int>(input.size()) : 0;
    const int frames = !input.empty() && !input[0].empty() ? static_cast<int>(input[0].size()) : o.frames;
    const int maxBlock = *std::max_element(o.blocks.begin(), o.blocks.end());
    if (o.activate) {
        for (const auto& [id, plain] : o.initial) {
            const int i = params::indexOf(id);
            if (i >= 0) engine->store().set(static_cast<size_t>(i), toHost(params::at(static_cast<size_t>(i)), plain));
        }
        for (const auto& [id, value] : o.properties) {
            const int i = properties::indexOf(id);
            if (i >= 0) engine->setProperty(static_cast<size_t>(i), value, true);  // a file loads, too
        }
        engine->setTuning(o.tuning);
        engine->setOffline(o.offline);
        engine->activate(o.sampleRate, maxBlock, product::info::kAudioInput ? channels : 0, channels);
    }
    engine->setMetering(o.metering);

    // Automation and events in one list, in time order; at one frame, the
    // order given.
    std::vector<EventAt> timeline;
    for (const Automation& a : o.automation) {
        const int i = params::indexOf(a.id);
        EventAt e{a.frame, {}};
        e.event.type = EngineEvent::Type::Value;
        e.event.index = i;
        e.event.value = i >= 0 ? toHost(params::at(static_cast<size_t>(i)), a.plain) : 0.0;
        timeline.push_back(e);
    }
    timeline.insert(timeline.end(), o.events.begin(), o.events.end());
    std::stable_sort(timeline.begin(), timeline.end(), [](const EventAt& a, const EventAt& b) { return a.frame < b.frame; });

    struct Source {
        const std::vector<EventAt>* list;
        size_t next;
        int start, end;
        bool next_(EngineEvent& e) {
            if (next >= list->size() || (*list)[next].frame >= end) return false;
            const EventAt& a = (*list)[next++];
            e = a.event;
            e.time = static_cast<uint32_t>(std::max(0, a.frame - start));
            return true;
        }
    };
    struct Events {
        Source* s;
        bool next(EngineEvent& out) { return s->next_(out); }
    };

    Signal out = product::info::kAudioOutput ? input : Signal{};
    if (product::info::kInstrument)
        for (auto& c : out) std::fill(c.begin(), c.end(), 0.0);
    const int auxBuses = product::info::kAuxOutputs;
    std::vector<Signal> aux(static_cast<size_t>(auxBuses), silence(channels, frames));
    const bool sidechain = product::info::kSidechain && !o.sidechain.empty();
    std::vector<double*> pointers(static_cast<size_t>(channels));
    std::vector<const double*> keys(static_cast<size_t>(channels));
    std::vector<std::vector<double*>> auxChannels(static_cast<size_t>(auxBuses), std::vector<double*>(static_cast<size_t>(channels)));
    std::vector<double* const*> auxBusPointers(static_cast<size_t>(auxBuses));
    if (o.sent) o.sent->clear();
    Source source{&timeline, 0, 0, 0};
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(o.blocks[static_cast<size_t>(b) % o.blocks.size()], frames - frame);
        for (int c = 0; c < channels; ++c) {
            pointers[static_cast<size_t>(c)] = out[static_cast<size_t>(c)].data() + frame;
            if (sidechain) keys[static_cast<size_t>(c)] = o.sidechain[static_cast<size_t>(c) % o.sidechain.size()].data() + frame;
            for (int k = 0; k < auxBuses; ++k)
                auxChannels[static_cast<size_t>(k)][static_cast<size_t>(c)] = aux[static_cast<size_t>(k)][static_cast<size_t>(c)].data() + frame;
        }
        for (int k = 0; k < auxBuses; ++k) auxBusPointers[static_cast<size_t>(k)] = auxChannels[static_cast<size_t>(k)].data();
        AudioBuses<double> io;
        io.in = product::info::kAudioInput ? pointers.data() : nullptr;
        io.out = channels ? pointers.data() : nullptr;
        io.sidechain = sidechain ? keys.data() : nullptr;
        io.aux = auxBuses ? auxBusPointers.data() : nullptr;
        source.start = frame;
        source.end = frame + n;
        Events events{&source};
        engine->process(io, n, events, nullptr);
        if (o.sent) {
            const NoteOutput& sent = engine->noteOutput();
            for (int k = 0; k < sent.count(); ++k)
                o.sent->push_back({frame + static_cast<int>(sent[k].time), sent[k].kind, sent[k].note, sent[k].midi});
        }
        frame += n;
    }
    if (o.aux) *o.aux = std::move(aux);
    return out;
}

// --- measurements ---------------------------------------------------------------------
//
//   CHECK_NEAR(test::gainDb(o, 1000.0), -3.01, 0.05);       // a sine through the product
//   CHECK(test::thd(out[0], 1000.0, 48000.0, 24000) < 1e-3); // harmonics 2-10 / fundamental
//   CHECK(test::measureLatency(o) == engine.latency());

// The amplitude of the component at `hz` in samples [from, end): a
// Hann-windowed DFT evaluated at exactly that frequency, so a steady sine
// reads its true amplitude whether or not it fits the window, and other
// components (harmonics, noise, another tone) barely leak into it.
inline double toneAmplitude(const std::vector<double>& x, double hz, double sampleRate, int from = 0) {
    const size_t start = static_cast<size_t>(std::max(0, from));
    if (start >= x.size()) return 0.0;
    const size_t n = x.size() - start;
    const double w = 2.0 * dsp::kPi * hz / sampleRate;
    double re = 0.0, im = 0.0, sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * dsp::kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n));
        const double phase = w * static_cast<double>(i);
        re += hann * x[start + i] * std::cos(phase);
        im -= hann * x[start + i] * std::sin(phase);
        sum += hann;
    }
    return 2.0 * std::hypot(re, im) / sum;
}

// Total harmonic distortion of a tone at `hz`: the RMS sum of harmonics 2 to
// `harmonics` (those below Nyquist) over the fundamental, as a ratio (1e-3
// is 0.1%, -60 dB).
inline double thd(const std::vector<double>& x, double hz, double sampleRate, int from = 0, int harmonics = 10) {
    const double fundamental = toneAmplitude(x, hz, sampleRate, from);
    double sum = 0.0;
    for (int k = 2; k <= harmonics && k * hz < sampleRate * 0.5; ++k) {
        const double a = toneAmplitude(x, k * hz, sampleRate, from);
        sum += a * a;
    }
    return fundamental > 0.0 ? std::sqrt(sum) / fundamental : INFINITY;
}

// The delay of `out` against `in`, in samples: the lag in [0, maxLag] at
// which they correlate most. `strength`, if given, receives that
// correlation normalised to [0, 1]: near 1 for a delayed copy, low when the
// output is no longer a version of the input.
inline int delayBetween(const std::vector<double>& in, const std::vector<double>& out, int maxLag,
                        double* strength = nullptr) {
    const size_t n = std::min(in.size(), out.size());
    int best = 0;
    double bestCorr = 0.0, inEnergy = 0.0, outEnergy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        inEnergy += in[i] * in[i];
        outEnergy += out[i] * out[i];
    }
    for (int lag = 0; lag <= maxLag && static_cast<size_t>(lag) < n; ++lag) {
        double c = 0.0;
        for (size_t i = static_cast<size_t>(lag); i < n; ++i) c += in[i - static_cast<size_t>(lag)] * out[i];
        if (std::fabs(c) > std::fabs(bestCorr)) {
            bestCorr = c;
            best = lag;
        }
    }
    if (strength) *strength = inEnergy > 0.0 && outEnergy > 0.0 ? std::fabs(bestCorr) / std::sqrt(inEnergy * outEnergy) : 0.0;
    return best;
}

// The gain at `hz` in dB, input channel to the same output channel: a sine
// of `amplitude` rendered through the options, measured after `settle`
// seconds (transients, smoothing, latency) for the `seconds` that follow.
inline double gainDb(const RenderOptions& o, double hz, double amplitude = 0.25, int channel = 0,
                     double settle = 0.25, double seconds = 0.5) {
    const int from = static_cast<int>(settle * o.sampleRate);
    const int frames = from + static_cast<int>(seconds * o.sampleRate);
    const int channels = product::info::kChannelLayouts[0];
    const Signal in = sine(channels, frames, hz, o.sampleRate, amplitude);
    const Signal out = render(in, o);
    const size_t c = static_cast<size_t>(channel);
    return dsp::gainToDb(toneAmplitude(out[c], hz, o.sampleRate, from), -300.0) -
           dsp::gainToDb(toneAmplitude(in[c], hz, o.sampleRate, from), -300.0);
}

// The THD of a sine of `amplitude` at `hz` rendered through the options.
inline double thdOf(const RenderOptions& o, double hz, double amplitude = 0.25, int channel = 0,
                    double settle = 0.25, double seconds = 0.5) {
    const int from = static_cast<int>(settle * o.sampleRate);
    const int frames = from + static_cast<int>(seconds * o.sampleRate);
    const Signal out = render(sine(product::info::kChannelLayouts[0], frames, hz, o.sampleRate, amplitude), o);
    return thd(out[static_cast<size_t>(channel)], hz, o.sampleRate, from);
}

// The latency an effect really has: noise rendered through the options, and
// the delay at which channel 0 of the output follows channel 0 of the input
// (see delayBetween; `strength` says how clearly).
inline int measureLatency(const RenderOptions& o, int maxLag = 4096, double* strength = nullptr) {
    const int frames = 2 * maxLag + 8192;
    const Signal in = noise(product::info::kChannelLayouts[0], frames, 11);
    const Signal out = render(in, o);
    return delayBetween(in[0], out[0], maxLag, strength);
}

// The response to a unit impulse on every input channel, `frames` long.
inline Signal impulseResponse(const RenderOptions& o, int frames) {
    Signal in = silence(product::info::kChannelLayouts[0], frames);
    for (auto& c : in) c[0] = 1.0;
    return render(in, o);
}

}  // namespace supple::test
