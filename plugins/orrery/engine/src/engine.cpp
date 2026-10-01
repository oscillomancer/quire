#include "orrery/engine.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <map>
#include <vector>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#define ORRERY_HAS_MXCSR 1
#endif

#include "dsp.h"
#include "filters.h"
#include "fx.h"
#include "modulators.h"
#include "oscillators.h"
#include "oversampling.h"
#include "supple/base/handoff.h"
#include "wavetables.h"

namespace supple::orrery {

using dsp::kTick;
using params::ParamSpec;
using units::Unit;

namespace {

constexpr int kVoicePool = Engine::kMaxVoices + 16;  // spares fade out stolen notes
constexpr int kMaxMods = 96;                         // modulated destinations per scope
constexpr std::uint32_t kFadeSamples = 64;           // base-rate samples
constexpr std::size_t kSourceCount = static_cast<std::size_t>(Source::Count);
constexpr int kMaxHeld = 128;       // every MIDI key
constexpr double kVoiceGain = 0.5;  // -6 dB internal headroom per voice

// --- program: a compiled patch -------------------------------------------------------

struct CRoute {
    Source src = Source::Amp;
    Source via = Source::Count;  // Count = none
    double amount = 0.0;
    double curve = 0.0;
};
struct CDest {
    std::uint16_t first = 0, count = 0;
};

struct Program {
    Structure st;
    std::vector<double> values;
    std::uint64_t seed = 0;
    int os = 2;
    std::array<bool, kSlotCount> has{};
    std::vector<CRoute> routes;  // grouped by destination
    std::vector<CDest> voiceDests, globalDests;
    std::vector<std::int16_t> voiceSlot, globalSlot;  // table index → mod slot or -1
    std::array<std::int16_t, kOscCount> pitchSlot{-1, -1, -1, -1};
    // Audio-rate routes [carrier][modulator].
    std::array<std::array<double, kOscCount>, kOscCount> fm{}, am{};
    std::array<std::array<Source, kOscCount>, kOscCount> fmVia{}, amVia{};
    std::array<int, kOscCount> order{0, 1, 2, 3};
    std::array<std::array<bool, kOscCount>, kOscCount> delayed{};
    std::array<std::array<float, kMaxSteps>, kStepCount> steps{};
    const WavetableBank* tables = nullptr;  // generated when a wavetable oscillator exists
    // Voices render stage by stage (oscillators, mixer, filters, drive, amp)
    // unless an audio-rate route reads a later oscillator's previous sample or
    // sync would read osc1 before it runs: those need the per-sample path.
    bool staged = true;
};

std::unique_ptr<Program> compileProgram(const Patch& patch) {
    auto p = std::make_unique<Program>();
    p->st = patch.body.structure;
    p->values = patch.body.values;
    p->seed = patch.meta.seed.value_or(0);
    p->os = oversampling(p->st.quality);
    for (const auto& t : p->st.osc)
        if (t == OscType::Wavetable) p->tables = &WavetableBank::get();  // main thread
    for (int s = 0; s < kSlotCount; ++s)
        p->has[static_cast<std::size_t>(s)] = p->st.has(static_cast<Slot>(s));
    p->voiceSlot.assign(paramCount(), -1);
    p->globalSlot.assign(paramCount(), -1);
    for (auto& row : p->fmVia) row.fill(Source::Count);
    for (auto& row : p->amVia) row.fill(Source::Count);

    // Group routes by destination (the patch orders them by source first).
    auto key = [](const Dest& d) { return static_cast<int>(d.kind) * 100000 + d.index; };
    std::map<int, std::vector<const Route*>> byDest;
    for (const Route& r : p->st.routes) byDest[key(r.dest)].push_back(&r);
    for (const auto& [k, list] : byDest) {
        const Dest d = list.front()->dest;
        if (d.kind == Dest::Kind::Fm || d.kind == Dest::Kind::Am) {
            for (const Route* r : list) {
                const int m = static_cast<int>(r->source) - static_cast<int>(Source::Osc1);
                if (d.kind == Dest::Kind::Fm) {
                    p->fm[d.index][static_cast<std::size_t>(m)] = r->amount;
                    p->fmVia[d.index][static_cast<std::size_t>(m)] = r->via.value_or(Source::Count);
                } else {
                    p->am[d.index][static_cast<std::size_t>(m)] = r->amount / 100.0;
                    p->amVia[d.index][static_cast<std::size_t>(m)] = r->via.value_or(Source::Count);
                }
            }
            continue;
        }
        const bool global = isGlobalDest(d, p->st);
        auto& dests = global ? p->globalDests : p->voiceDests;
        if (dests.size() >= static_cast<std::size_t>(kMaxMods)) continue;
        const auto slot = static_cast<std::int16_t>(dests.size());
        CDest cd;
        cd.first = static_cast<std::uint16_t>(p->routes.size());
        cd.count = static_cast<std::uint16_t>(list.size());
        for (const Route* r : list)
            p->routes.push_back({r->source, r->via.value_or(Source::Count), r->amount, r->curve});
        dests.push_back(cd);
        if (d.kind == Dest::Kind::Pitch)
            p->pitchSlot[d.index] = slot;
        else
            (global ? p->globalSlot : p->voiceSlot)[d.index] = slot;
    }

    // Oscillator order: modulators before carriers; edges in cycles read the
    // modulator's previous sample.
    std::array<std::array<bool, kOscCount>, kOscCount> edge{};  // [carrier][modulator]
    for (int c = 0; c < kOscCount; ++c)
        for (int m = 0; m < kOscCount; ++m)
            edge[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] =
                p->fm[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] != 0.0 ||
                p->am[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] != 0.0;
    std::array<bool, kOscCount> placed{};
    int n = 0;
    for (int pass = 0; pass < kOscCount && n < kOscCount; ++pass) {
        for (int c = 0; c < kOscCount; ++c) {
            if (placed[static_cast<std::size_t>(c)]) continue;
            bool ready = true;
            for (int m = 0; m < kOscCount; ++m)
                if (m != c && edge[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] &&
                    !placed[static_cast<std::size_t>(m)])
                    ready = false;
            if (ready) {
                placed[static_cast<std::size_t>(c)] = true;
                p->order[static_cast<std::size_t>(n++)] = c;
            }
        }
    }
    for (int c = 0; c < kOscCount; ++c)
        if (!placed[static_cast<std::size_t>(c)]) p->order[static_cast<std::size_t>(n++)] = c;
    std::array<int, kOscCount> rank{};
    for (int i = 0; i < kOscCount; ++i)
        rank[static_cast<std::size_t>(p->order[static_cast<std::size_t>(i)])] = i;
    for (int c = 0; c < kOscCount; ++c)
        for (int m = 0; m < kOscCount; ++m)
            p->delayed[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] =
                rank[static_cast<std::size_t>(m)] >= rank[static_cast<std::size_t>(c)];

    for (int c = 0; c < kOscCount; ++c)
        for (int m = 0; m < kOscCount; ++m) {
            const auto uc = static_cast<std::size_t>(c), um = static_cast<std::size_t>(m);
            if ((p->fm[uc][um] != 0.0 || p->am[uc][um] != 0.0) && p->delayed[uc][um])
                p->staged = false;
        }
    if (p->order[0] != 0)
        for (int o = 1; o < kOscCount; ++o)
            if (p->st.osc[static_cast<std::size_t>(o)] == OscType::Va) p->staged = false;

    for (int s = 0; s < kStepCount; ++s) {
        const auto& v = p->st.stepValues[static_cast<std::size_t>(s)];
        for (std::size_t k = 0; k < v.size() && k < static_cast<std::size_t>(kMaxSteps); ++k)
            p->steps[static_cast<std::size_t>(s)][k] = static_cast<float>(v[k] / 100.0);
    }
    return p;
}

// Whether two programs need the same voice DSP for a slot.
bool sameOsc(const Program& a, const Program& b, int o) {
    return a.st.osc[static_cast<std::size_t>(o)] == b.st.osc[static_cast<std::size_t>(o)];
}
bool sameFilter(const Program& a, const Program& b, int f) {
    return a.st.filter[static_cast<std::size_t>(f)] == b.st.filter[static_cast<std::size_t>(f)];
}

// --- parameter ramps (automation smoothing, block-size independent) ----------------------

struct Ramp {
    double from = 0.0, to = 0.0;
    std::int64_t t0 = 0;
    double len = 0.0;  // samples; 0 = step
    double value(std::int64_t t) const SUPPLE_RT {
        if (len <= 0.0 || t >= t0 + static_cast<std::int64_t>(len)) return to;
        if (t <= t0) return from;
        return from + (to - from) * static_cast<double>(t - t0) / len;
    }
    void set(double target, std::int64_t t, double samples) SUPPLE_RT {
        from = value(t);
        to = target;
        t0 = t;
        len = samples;
    }
    void snap(double v) SUPPLE_RT {
        from = to = v;
        len = 0.0;
    }
};

// --- voices --------------------------------------------------------------------------------

struct Targets {
    std::array<double, kOscCount> inc{}, gainL{}, gainR{}, width{}, shape{}, fb{}, position{};
    // Each oscillator's level alone (no pan): FM/AM modulators are heard after it.
    std::array<double, kOscCount> modGain{};
    std::array<double, kFilterCount> g{}, k{}, drive{}, level{}, delay{}, feedback{}, damping{};
    std::array<Formant::Coefs, kFilterCount> formant{};
    double bal1 = 1.0, bal2 = 1.0;
    double driveGain = 1.0, driveBias = 0.0, driveMix = 1.0, driveComp = 1.0;
    double ampL = 0.0, ampR = 0.0;
};

struct Voice {
    enum class State : std::uint8_t { Free, Active, Fading };
    State state = State::Free;
    int key = 60, channel = 0;
    std::int32_t noteId = -1;
    std::uint64_t serial = 0;
    int unison = 0, unisonCount = 1;
    bool gate = false, sustained = false;
    double velocity = 0.0, releaseVelocity = 0.0, random = 0.0;
    double glideOffset = 0.0, glideStep = 0.0;       // semitones, per base sample
    double bend = 0.0, pressure = 0.0, slide = 0.0;  // per-note (MPE / poly pressure)
    std::uint32_t tickPos = 0;
    bool fresh = true;
    double fade = 1.0, fadeStep = 0.0;

    Rng rng{0};
    std::array<Envelope, kEnvCount> env{};
    std::array<Lfo, kLfoCount> lfo{};
    std::array<StepSeq, kStepCount> step{};
    Lfo driftPitch, driftCutoff;
    std::array<VaOsc, kOscCount> va{};
    std::array<FmOsc, kOscCount> fmo{};
    std::array<WtOsc, kOscCount> wt{};
    std::array<NoiseOsc, kOscCount> noise{};
    std::array<std::array<Svf, 2>, kFilterCount> svf{};
    std::array<std::array<Ladder, 2>, kFilterCount> ladder{};
    std::array<std::array<Comb, 2>, kFilterCount> comb{};
    std::array<std::array<Formant, 2>, kFilterCount> formant{};

    std::array<double, kMaxMods> mod{};
    std::array<double, kOscCount> prevOut{};
    std::array<std::array<double, kOscCount>, kOscCount> fmIndex{}, amDepth{};
    // Per-sample ramps across a control tick.
    std::array<dsp::Lerp, kOscCount> inc{}, gainL{}, gainR{}, width{}, shape{}, fb{}, position{};
    std::array<dsp::Lerp, kOscCount> modGain{};
    std::array<dsp::Lerp, kFilterCount> g{}, k{}, drive{}, level{}, delay{}, feedback{}, damping{};
    std::array<Formant::Coefs, kFilterCount> formantCoefs{};
    dsp::Lerp bal1, bal2, driveGain, driveBias, driveMix, driveComp, ampL, ampR;
    bool monoPath = false;  // the pre-amp signal is identical in both channels
};

struct Frame {
    std::array<double, kSourceCount> src{};
    std::array<double, kMaxMods> mod{};
};

}  // namespace

// --- engine implementation ---------------------------------------------------------------------

// The longest voice segment (a tick at high quality), and one voice's staged
// buffers for it.
constexpr std::size_t kSeg = static_cast<std::size_t>(kTick) * 4;
struct StageBuf {
    int channels = 1;
    bool mono = true;
    double in1[2][kSeg], in2[2][kSeg], direct[2][kSeg], b1[kSeg], b2[kSeg];
    double co[kFilterCount][7][kSeg];
    double dg[kSeg], db[kSeg], dm[kSeg], dc[kSeg], al[kSeg], ar[kSeg];
    double y[2][kSeg], f1[kSeg], f2[kSeg], x2[kSeg];
};
constexpr int kGroupMax = 8;  // unison voices of one note rendered together

struct Engine::Impl {
    double sr = 48000.0;
    std::uint32_t maxBlock = 0;
    RtHandoff<Program> handoff;
    Program* prog = nullptr;
    params::Table tbl = table();

    std::vector<Ramp> ramps;
    Ramp modwheel, breath, expression, slide, pitchbend, aftertouch;
    bool sustainPedal = false;
    std::int64_t now = 0;
    double tempo = 120.0;

    std::array<Voice, kVoicePool> voices{};
    std::uint64_t serial = 0;
    double lastPitch = -1.0;
    std::array<int, kMaxHeld> held{};
    int heldCount = 0;
    std::uint64_t monoSerial = 0;  // the sounding mono/legato note (0 = none)

    std::array<Lfo, kLfoCount> glfo{};
    std::array<StepSeq, kStepCount> gstep{};
    std::array<bool, kLfoCount> monoRestart{};
    std::array<bool, kStepCount> monoStepRestart{};
    std::vector<Frame> frames;  // [0] = last frame before the block
    std::int64_t firstTick = 0;
    std::int64_t lastFrameTick = -1;

    std::vector<float> busL, busR, outL, outR;
    std::array<StageBuf, kGroupMax> stages;  // staged voice buffers (one per group member)
    std::vector<float> combMemory;
    // One decimator pair per oversampling factor (1, 2, 4), all prepared up
    // front, so a quality change never reconfigures a running decimator.
    std::array<Decimator, 3> decL, decR;
    int decFactor = 0;
    static std::size_t decIndex(int os) { return os == 1 ? 0 : os == 2 ? 1 : 2; }
    FxRack fx;
    dsp::Lerp outGain;
    std::uint64_t quiet = 0;
    std::array<double, 8> fxValues{};

    // ---------------------------------------------------------------------------------------
    double base(std::size_t i, std::int64_t t) const SUPPLE_RT { return ramps[i].value(t); }

    double applyMod(std::size_t i, double b, double m) const SUPPLE_RT {
        if (m == 0.0) return b;
        const ParamSpec& p = tbl[i];
        double v = 0.0;
        switch (p.unit) {
            case Unit::Hz:
                v = b * std::exp2(m);
                break;
            case Unit::st:
                v = b + m;
                break;
            case Unit::ct:
                v = b + 100.0 * m;
                break;
            case Unit::dB:
                v = b + m;
                break;
            default:
                return params::fromNormalized(
                    p, std::clamp(params::toNormalized(p, b) + m / 100.0, 0.0, 1.0));
        }
        return std::clamp(v, p.min, p.max);
    }

    const Frame& frameAt(std::int64_t t) const SUPPLE_RT {
        const std::int64_t tick = t / static_cast<std::int64_t>(kTick);
        const std::int64_t k = std::clamp<std::int64_t>(
            tick - firstTick + 1, 0, static_cast<std::int64_t>(frames.size()) - 1);
        return frames[static_cast<std::size_t>(k)];
    }

    static double sourceValue(const CRoute& r,
                              const std::array<double, kSourceCount>& src) SUPPLE_RT {
        double v = dsp::sourceCurve(src[static_cast<std::size_t>(r.src)], r.curve);
        if (r.via != Source::Count) v *= src[static_cast<std::size_t>(r.via)];
        return v * r.amount;
    }

    // Effective value for a voice at time t (base + voice mods + global mods).
    double eff(const Voice& v, std::size_t i, std::int64_t t, const Frame& f) const SUPPLE_RT {
        double m = 0.0;
        if (const auto s = prog->voiceSlot[i]; s >= 0) m += v.mod[static_cast<std::size_t>(s)];
        if (const auto s = prog->globalSlot[i]; s >= 0) m += f.mod[static_cast<std::size_t>(s)];
        return applyMod(i, base(i, t), m);
    }
    double geff(std::size_t i, std::int64_t t, const Frame& f) const SUPPLE_RT {
        const auto s = prog->globalSlot[i];
        return applyMod(i, base(i, t), s >= 0 ? f.mod[static_cast<std::size_t>(s)] : 0.0);
    }

    double syncHz(NoteValue nv) const SUPPLE_RT {
        return tempo / 60.0 / std::max(1e-6, noteQuarters(nv));
    }

    LfoParams lfoParams(int l, const Voice* v, std::int64_t t, const Frame& f) const SUPPLE_RT {
        const Slot s = lfo(l);
        auto get = [&](LfoF field) {
            const std::size_t i = index(s, field);
            return v ? eff(*v, i, t, f) : geff(i, t, f);
        };
        LfoParams p;
        p.wave = static_cast<LfoWave>(get(LfoF::wave));
        const auto sync = static_cast<NoteValue>(get(LfoF::sync));
        p.hz = sync == NoteValue::Off ? get(LfoF::rate) : syncHz(sync);
        p.phase = get(LfoF::phase) / 360.0;
        p.fade = get(LfoF::fade) * 1e-3 * sr;
        p.unipolar = static_cast<Polarity>(get(LfoF::polarity)) == Polarity::Unipolar;
        return p;
    }

    double stepSamples(int s, const Voice* v, std::int64_t t, const Frame& f) const SUPPLE_RT {
        const std::size_t i = index(step(s), StepF::sync);
        const double choice = v ? eff(*v, i, t, f) : geff(i, t, f);
        const auto nv = static_cast<NoteValue>(static_cast<int>(choice) + 1);
        return noteQuarters(nv) * 60.0 / tempo * sr;
    }

    EnvParams envParams(const Voice& v, int e, std::int64_t t, const Frame& f) const SUPPLE_RT {
        const Slot s = env(e);
        auto get = [&](EnvF field) { return eff(v, index(s, field), t, f); };
        const double ms = sr * 1e-3;
        return {get(EnvF::delay) * ms,
                get(EnvF::attack) * ms,
                get(EnvF::hold) * ms,
                get(EnvF::decay) * ms,
                get(EnvF::sustain) / 100.0,
                get(EnvF::release) * ms,
                get(EnvF::attack_curve) / 100.0,
                get(EnvF::decay_curve) / 100.0,
                get(EnvF::release_curve) / 100.0};
    }

    // --- global control ticks -----------------------------------------------------------------
    void globalTick(std::int64_t g) SUPPLE_RT {
        const std::int64_t tick = g / static_cast<std::int64_t>(kTick);
        const std::int64_t k = tick - firstTick + 1;
        if (k < 1 || k >= static_cast<std::int64_t>(frames.size())) return;
        Frame& f = frames[static_cast<std::size_t>(k)];
        const Frame& prev = frames[static_cast<std::size_t>(k - 1)];
        f.src.fill(0.0);
        f.src[static_cast<std::size_t>(Source::Modwheel)] = modwheel.value(g);
        f.src[static_cast<std::size_t>(Source::Breath)] = breath.value(g);
        f.src[static_cast<std::size_t>(Source::Expression)] = expression.value(g);
        f.src[static_cast<std::size_t>(Source::Pitchbend)] = pitchbend.value(g);
        f.src[static_cast<std::size_t>(Source::Aftertouch)] = aftertouch.value(g);
        f.src[static_cast<std::size_t>(Source::Slide)] = slide.value(g);
        f.src[static_cast<std::size_t>(Source::Pressure)] = aftertouch.value(g);
        for (int m = 0; m < kMacroCount; ++m)
            f.src[static_cast<std::size_t>(Source::Macro1) + static_cast<std::size_t>(m)] =
                base(index(macro(m), MacroF::value), g) / 100.0;
        // Global LFOs and steps use the previous frame's global mods for their
        // own parameters (one tick of lag), then advance.
        for (int l = 0; l < kLfoCount; ++l) {
            if (!prog->st.lfo[static_cast<std::size_t>(l)] ||
                prog->st.lfoRetrigger[static_cast<std::size_t>(l)] == Retrigger::Voice)
                continue;
            const LfoParams p = lfoParams(l, nullptr, g, prev);
            if (monoRestart[static_cast<std::size_t>(l)]) {
                glfo[static_cast<std::size_t>(l)].restart(p);
                monoRestart[static_cast<std::size_t>(l)] = false;
            }
            f.src[static_cast<std::size_t>(Source::Lfo1) + static_cast<std::size_t>(l)] =
                glfo[static_cast<std::size_t>(l)].advance(kTick, sr, p);
        }
        for (int s = 0; s < kStepCount; ++s) {
            if (!prog->st.step[static_cast<std::size_t>(s)] ||
                prog->st.stepRetrigger[static_cast<std::size_t>(s)] == Retrigger::Voice)
                continue;
            if (monoStepRestart[static_cast<std::size_t>(s)]) {
                gstep[static_cast<std::size_t>(s)].restart();
                monoStepRestart[static_cast<std::size_t>(s)] = false;
            }
            f.src[static_cast<std::size_t>(Source::Step1) + static_cast<std::size_t>(s)] =
                gstep[static_cast<std::size_t>(s)].advance(
                    kTick, stepSamples(s, nullptr, g, prev),
                    prog->steps[static_cast<std::size_t>(s)],
                    static_cast<int>(geff(index(step(s), StepF::length), g, prev)),
                    geff(index(step(s), StepF::glide), g, prev) / 100.0);
        }
        for (std::size_t d = 0; d < prog->globalDests.size(); ++d) {
            const CDest& cd = prog->globalDests[d];
            double sum = 0.0;
            for (std::uint16_t r = 0; r < cd.count; ++r)
                sum += sourceValue(prog->routes[cd.first + r], f.src);
            f.mod[d] = sum;
        }
        lastFrameTick = tick;
    }

    // --- voices ---------------------------------------------------------------------------------
    double notePitch(const Voice& v, std::int64_t t, const Frame& f) const SUPPLE_RT {
        const double bendRange = base(index(kVoice, VoiceF::bend_range), t);
        const double unisonCt = v.unisonCount > 1
                                    ? eff(v, index(kVoice, VoiceF::unison_detune), t, f) *
                                          (2.0 * v.unison / (v.unisonCount - 1) - 1.0)
                                    : 0.0;
        return v.key + v.glideOffset +
               f.src[static_cast<std::size_t>(Source::Pitchbend)] * bendRange + v.bend +
               unisonCt / 100.0;
    }

    void computeTargets(Voice& v, std::int64_t t, std::uint32_t advance, Targets& out) SUPPLE_RT {
        const Program& P = *prog;
        const Frame& f = frameAt(t);
        const double dt = advance;
        const std::int64_t te = t + advance;  // values at the end of the tick
        // Modulators.
        std::array<double, kSourceCount> src = f.src;
        for (int e = 0; e < kEnvCount; ++e)
            if (P.st.env[static_cast<std::size_t>(e)])
                src[static_cast<std::size_t>(e)] =
                    v.env[static_cast<std::size_t>(e)].advance(dt, envParams(v, e, te, f));
        for (int l = 0; l < kLfoCount; ++l)
            if (P.st.lfo[static_cast<std::size_t>(l)] &&
                P.st.lfoRetrigger[static_cast<std::size_t>(l)] == Retrigger::Voice)
                src[static_cast<std::size_t>(Source::Lfo1) + static_cast<std::size_t>(l)] =
                    v.lfo[static_cast<std::size_t>(l)].advance(dt, sr, lfoParams(l, &v, te, f));
        for (int s = 0; s < kStepCount; ++s)
            if (P.st.step[static_cast<std::size_t>(s)] &&
                P.st.stepRetrigger[static_cast<std::size_t>(s)] == Retrigger::Voice)
                src[static_cast<std::size_t>(Source::Step1) + static_cast<std::size_t>(s)] =
                    v.step[static_cast<std::size_t>(s)].advance(
                        dt, stepSamples(s, &v, te, f), P.steps[static_cast<std::size_t>(s)],
                        static_cast<int>(eff(v, index(step(s), StepF::length), te, f)),
                        eff(v, index(step(s), StepF::glide), te, f) / 100.0);
        src[static_cast<std::size_t>(Source::Velocity)] = v.velocity;
        src[static_cast<std::size_t>(Source::ReleaseVelocity)] = v.releaseVelocity;
        src[static_cast<std::size_t>(Source::Key)] =
            v.key < 60 ? (v.key - 60) / 60.0 : (v.key - 60) / 67.0;
        src[static_cast<std::size_t>(Source::Random)] = v.random;
        src[static_cast<std::size_t>(Source::Gate)] = v.gate || v.sustained ? 1.0 : 0.0;
        if (v.pressure > 0.0) src[static_cast<std::size_t>(Source::Pressure)] = v.pressure;
        if (v.slide > 0.0) src[static_cast<std::size_t>(Source::Slide)] = v.slide;
        // Voice modulation sums.
        for (std::size_t d = 0; d < P.voiceDests.size(); ++d) {
            const CDest& cd = P.voiceDests[d];
            double sum = 0.0;
            for (std::uint16_t r = 0; r < cd.count; ++r)
                sum += sourceValue(P.routes[cd.first + r], src);
            v.mod[d] = sum;
        }
        for (int c = 0; c < kOscCount; ++c)
            for (int m = 0; m < kOscCount; ++m) {
                const double a = P.fm[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)];
                const Source via =
                    P.fmVia[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)];
                v.fmIndex[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] =
                    a * (via == Source::Count ? 1.0 : src[static_cast<std::size_t>(via)]) /
                    dsp::kTwoPi;
                const Source amVia =
                    P.amVia[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)];
                v.amDepth[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] =
                    P.am[static_cast<std::size_t>(c)][static_cast<std::size_t>(m)] *
                    (amVia == Source::Count ? 1.0 : src[static_cast<std::size_t>(amVia)]);
            }
        // Glide and drift.
        if (v.glideOffset != 0.0) {
            const double next = v.glideOffset - v.glideStep * dt;
            v.glideOffset = (v.glideOffset > 0.0) == (next > 0.0) ? next : 0.0;
        }
        const double drift = eff(v, index(kVoice, VoiceF::drift), te, f) / 100.0;
        LfoParams dp{LfoWave::SmoothRandom, 0.3, 0.0, 0.0, false};
        const double driftCt = drift * 5.0 * v.driftPitch.advance(dt, sr, dp);
        dp.hz = 0.23;
        const double driftCut = 1.0 + drift * 0.03 * v.driftCutoff.advance(dt, sr, dp);

        const double fs = sr * P.os;
        const double pitch = notePitch(v, te, f) + driftCt / 100.0;
        // Oscillators.
        for (int o = 0; o < kOscCount; ++o) {
            const auto& type = P.st.osc[static_cast<std::size_t>(o)];
            if (!type) continue;
            const Slot s = osc(o);
            auto get = [&](OscF field) { return eff(v, index(s, field), te, f); };
            const double pmod =
                P.pitchSlot[static_cast<std::size_t>(o)] >= 0
                    ? v.mod[static_cast<std::size_t>(P.pitchSlot[static_cast<std::size_t>(o)])]
                    : 0.0;
            double hz = 0.0;
            switch (*type) {
                case OscType::Sub:
                    hz = dsp::pitchToHz(pitch + 12.0 * get(OscF::octave) + pmod);
                    break;
                case OscType::Fm: {
                    const auto mode = static_cast<RatioMode>(get(OscF::ratio_mode));
                    const double tune = get(OscF::coarse) + get(OscF::fine) / 100.0 + pmod;
                    if (mode == RatioMode::Fixed) {
                        hz = get(OscF::fixed_freq) * std::exp2(tune / 12.0);
                    } else {
                        double ratio = get(OscF::ratio);
                        if (mode == RatioMode::Harmonic)
                            ratio = std::max(0.25, std::round(ratio * 4.0) / 4.0);
                        hz = dsp::pitchToHz(pitch + tune) * ratio;
                    }
                    break;
                }
                default:
                    hz = dsp::pitchToHz(pitch + get(OscF::coarse) + get(OscF::fine) / 100.0 + pmod);
            }
            out.inc[static_cast<std::size_t>(o)] = std::min(hz / fs, 0.49);
            const auto pg = dsp::pan(get(OscF::pan) / 100.0);
            const double lvl = dsp::levelGain(get(OscF::level)) * std::numbers::sqrt2;
            out.modGain[static_cast<std::size_t>(o)] = dsp::levelGain(get(OscF::level));
            out.gainL[static_cast<std::size_t>(o)] = lvl * pg.l;
            out.gainR[static_cast<std::size_t>(o)] = lvl * pg.r;
            out.width[static_cast<std::size_t>(o)] = get(OscF::width) / 100.0;
            out.shape[static_cast<std::size_t>(o)] = get(OscF::shape) / 100.0;
            out.fb[static_cast<std::size_t>(o)] = get(OscF::feedback) / 100.0;
            out.position[static_cast<std::size_t>(o)] = get(OscF::position) / 100.0;
        }
        // Filters.
        const double keyOct = (pitch - 60.0) / 12.0;
        for (int fi = 0; fi < kFilterCount; ++fi) {
            const auto& type = P.st.filter[static_cast<std::size_t>(fi)];
            if (!type) continue;
            const Slot s = filter(fi);
            auto get = [&](FilterF field) { return eff(v, index(s, field), te, f); };
            const double track = get(FilterF::keytrack) / 100.0;
            const double cutoff = std::clamp(
                get(FilterF::cutoff) * std::exp2(keyOct * track) * driftCut, 20.0, 20000.0);
            const auto u = static_cast<std::size_t>(fi);
            out.g[u] = tptG(cutoff, fs);
            const double res = get(FilterF::resonance) / 100.0;
            // Ladder: feedback 0..4 (self-oscillation at 4). SVF: damping
            // k = 2(1-r)² — perceptual (40 % ≈ Q 1.4, 70 % ≈ Q 5.6, 100 % rings).
            out.k[u] = *type == FilterType::Ladder
                           ? 4.0 * res
                           : std::max(0.005, 2.0 * (1.0 - res) * (1.0 - res));
            out.drive[u] = dsp::dbToGain(get(FilterF::drive));
            out.level[u] = dsp::levelGain(get(FilterF::level));
            out.delay[u] = fs / cutoff;
            out.feedback[u] = get(FilterF::feedback) / 100.0;
            out.damping[u] = get(FilterF::damping) / 100.0;
            if (*type == FilterType::Formant)
                out.formant[u] = Formant::coefs(get(FilterF::vowel),
                                                get(FilterF::shift) + 12.0 * keyOct * track, fs);
        }
        const double bal = eff(v, index(kFilters, FiltersF::balance), te, f) / 100.0;
        out.bal1 = std::min(1.0, 1.0 - bal);
        out.bal2 = std::min(1.0, 1.0 + bal);
        if (P.st.drive != DriveType::Off) {
            out.driveGain = dsp::dbToGain(eff(v, index(kDrive, DriveF::amount), te, f));
            out.driveBias = eff(v, index(kDrive, DriveF::bias), te, f) / 100.0;
            out.driveMix = eff(v, index(kDrive, DriveF::mix), te, f) / 100.0;
            out.driveComp = 1.0 / std::sqrt(std::max(1.0, out.driveGain));
        }
        // Amp: envelope × level × velocity, pan with unison spread.
        const double sens = eff(v, index(kAmp, AmpF::velocity), te, f) / 100.0;
        const double velDb = -(1.0 - v.velocity) * 40.0 * sens;
        const double amp =
            src[0] * dsp::levelGain(eff(v, index(kAmp, AmpF::level), te, f)) * dsp::dbToGain(velDb);
        double p = eff(v, index(kAmp, AmpF::pan), te, f) / 100.0;
        if (v.unisonCount > 1)
            p += eff(v, index(kVoice, VoiceF::unison_spread), te, f) / 100.0 *
                 (2.0 * v.unison / (v.unisonCount - 1) - 1.0);
        const auto pg = dsp::pan(p);
        // Headroom: one voice with one oscillator at 0 dB peaks at -6 dBFS;
        // unison voices are normalized by 1/sqrt(count).
        const double stage = kVoiceGain / std::sqrt(static_cast<double>(v.unisonCount));
        out.ampL = amp * pg.l * std::numbers::sqrt2 * stage;
        out.ampR = amp * pg.r * std::numbers::sqrt2 * stage;
    }

    void applyTargets(Voice& v, const Targets& t, bool snap) SUPPLE_RT {
        const double n = static_cast<double>(kTick * static_cast<std::uint32_t>(prog->os));
        auto go = [&](dsp::Lerp& l, double to) {
            if (snap)
                l.snap(to);
            else
                l.set(l.value, to, n);
        };
        for (std::size_t o = 0; o < kOscCount; ++o) {
            go(v.inc[o], t.inc[o]);
            go(v.gainL[o], t.gainL[o]);
            go(v.gainR[o], t.gainR[o]);
            go(v.modGain[o], t.modGain[o]);
            go(v.width[o], t.width[o]);
            go(v.shape[o], t.shape[o]);
            go(v.fb[o], t.fb[o]);
            go(v.position[o], t.position[o]);
        }
        for (std::size_t fi = 0; fi < kFilterCount; ++fi) {
            go(v.g[fi], t.g[fi]);
            go(v.k[fi], t.k[fi]);
            go(v.drive[fi], t.drive[fi]);
            go(v.level[fi], t.level[fi]);
            go(v.delay[fi], t.delay[fi]);
            go(v.feedback[fi], t.feedback[fi]);
            go(v.damping[fi], t.damping[fi]);
            v.formantCoefs[fi] = t.formant[fi];
        }
        go(v.bal1, t.bal1);
        go(v.bal2, t.bal2);
        go(v.driveGain, t.driveGain);
        go(v.driveBias, t.driveBias);
        go(v.driveMix, t.driveMix);
        go(v.driveComp, t.driveComp);
        go(v.ampL, t.ampL);
        go(v.ampR, t.ampR);
    }

    void voiceTick(Voice& v, std::int64_t t) SUPPLE_RT {
        Targets tg;
        if (v.fresh) {
            computeTargets(v, t, 0, tg);
            applyTargets(v, tg, true);
            v.fresh = false;
        }
        computeTargets(v, t, kTick, tg);
        applyTargets(v, tg, false);
    }

    double filterSample(Voice& v, int fi, int ch, double x) SUPPLE_RT {
        const auto u = static_cast<std::size_t>(fi);
        const auto c = static_cast<std::size_t>(ch);
        const double g = v.g[u].value, k = v.k[u].value, drive = v.drive[u].value;
        return filterWith(v, fi, c, x, g, k, drive, v.delay[u].value, v.feedback[u].value,
                          v.damping[u].value, v.level[u].value);
    }

    // One filter sample with explicit coefficients (the staged path passes
    // the same per-sample values the Lerps would give).
    double filterWith(Voice& v, int fi, std::size_t c, double x, double g, double k, double drive,
                      double delay, double feedback, double damping, double level) SUPPLE_RT {
        const auto u = static_cast<std::size_t>(fi);
        double y = x;
        switch (*prog->st.filter[u]) {
            case FilterType::Ladder: {
                const bool db24 =
                    static_cast<Slope>(ramps[index(filter(fi), FilterF::slope)].to) == Slope::Db24;
                y = v.ladder[u][c].next(x, g, k, drive, db24);
                break;
            }
            case FilterType::Svf: {
                const auto mode = static_cast<SvfMode>(ramps[index(filter(fi), FilterF::mode)].to);
                const double in = drive > 1.0001 ? dsp::fastTanh(x * drive) / std::sqrt(drive) : x;
                y = v.svf[u][c].next(in, g, k, mode);
                break;
            }
            case FilterType::Comb:
                y = v.comb[u][c].next(
                    drive > 1.0001 ? dsp::fastTanh(x * drive) / std::sqrt(drive) : x, delay,
                    feedback * 0.99, damping);
                break;
            case FilterType::Formant:
                y = v.formant[u][c].next(
                    drive > 1.0001 ? dsp::fastTanh(x * drive) / std::sqrt(drive) : x,
                    v.formantCoefs[u]);
                break;
        }
        return y * level;
    }

    // One filter over a buffer, the type decided once per segment.
    void filterRun(Voice& v, int fi, std::size_t c, const double* x, double* y, std::uint32_t n,
                   const double* const* co) SUPPLE_RT {
        const auto u = static_cast<std::size_t>(fi);
        const double *g = co[0], *k = co[1], *drv = co[2], *lvl = co[3], *dly = co[4], *fb = co[5],
                     *dmp = co[6];
        switch (*prog->st.filter[u]) {
            case FilterType::Ladder: {
                const bool db24 =
                    static_cast<Slope>(ramps[index(filter(fi), FilterF::slope)].to) == Slope::Db24;
                Ladder& f = v.ladder[u][c];
                for (std::uint32_t s = 0; s < n; ++s)
                    y[s] = f.next(x[s], g[s], k[s], drv[s], db24) * lvl[s];
                break;
            }
            case FilterType::Svf: {
                const auto mode = static_cast<SvfMode>(ramps[index(filter(fi), FilterF::mode)].to);
                Svf& f = v.svf[u][c];
                for (std::uint32_t s = 0; s < n; ++s) {
                    const double in =
                        drv[s] > 1.0001 ? dsp::fastTanh(x[s] * drv[s]) / std::sqrt(drv[s]) : x[s];
                    y[s] = f.next(in, g[s], k[s], mode) * lvl[s];
                }
                break;
            }
            case FilterType::Comb: {
                Comb& f = v.comb[u][c];
                for (std::uint32_t s = 0; s < n; ++s) {
                    const double in =
                        drv[s] > 1.0001 ? dsp::fastTanh(x[s] * drv[s]) / std::sqrt(drv[s]) : x[s];
                    y[s] = f.next(in, dly[s], fb[s] * 0.99, dmp[s]) * lvl[s];
                }
                break;
            }
            case FilterType::Formant: {
                Formant& f = v.formant[u][c];
                for (std::uint32_t s = 0; s < n; ++s) {
                    const double in =
                        drv[s] > 1.0001 ? dsp::fastTanh(x[s] * drv[s]) / std::sqrt(drv[s]) : x[s];
                    y[s] = f.next(in, v.formantCoefs[u]) * lvl[s];
                }
                break;
            }
        }
    }

    // Stage-by-stage rendering: oscillators (in processing order), mixer,
    // filters, drive, amp — the same arithmetic per sample, in the same order,
    // as the per-sample path (bit-identical output), with every per-type and
    // per-structure decision made once per segment.
    // Stage 1 of the staged path: oscillators, mixer, and every per-sample
    // coefficient (filters, drive, amp) from the Lerps.
    void stagePre(Voice& v, std::uint32_t n, StageBuf& sb) SUPPLE_RT {
        const Program& P = *prog;
        const double wtLimit = wavetableLimit(sr * P.os);
        const bool sync1 = P.st.osc[0].has_value();
        bool mono = true;
        for (std::size_t o = 0; o < kOscCount; ++o)
            if (P.st.osc[o] &&
                (v.gainL[o].value != v.gainR[o].value || v.gainL[o].step != v.gainR[o].step))
                mono = false;
        if (!mono && v.monoPath)
            for (std::size_t fi = 0; fi < kFilterCount; ++fi) {
                v.svf[fi][1] = v.svf[fi][0];
                v.ladder[fi][1] = v.ladder[fi][0];
                v.comb[fi][1].copyState(v.comb[fi][0]);
                v.formant[fi][1] = v.formant[fi][0];
            }
        v.monoPath = mono;
        sb.mono = mono;
        sb.channels = mono ? 1 : 2;

        // Oscillators. A modulator is heard after its level (synth spec §5.4): mg
        // is each oscillator's level ramp, advanced every sample like the mixer's.
        double out[kOscCount][kSeg];
        double mg[kOscCount][kSeg];
        double wrap[kSeg];
        for (std::size_t o = 0; o < kOscCount; ++o)
            if (P.st.osc[o])
                for (std::uint32_t s = 0; s < n; ++s) mg[o][s] = v.modGain[o].next();
        for (int oi = 0; oi < kOscCount; ++oi) {
            const int o = P.order[static_cast<std::size_t>(oi)];
            const auto uo = static_cast<std::size_t>(o);
            const auto& type = P.st.osc[uo];
            if (!type) continue;
            int fmN = 0, amN = 0;
            std::array<std::size_t, kOscCount> fmM{}, amM{};
            for (std::size_t m = 0; m < kOscCount; ++m) {
                if (v.fmIndex[uo][m] != 0.0) fmM[static_cast<std::size_t>(fmN++)] = m;
                if (P.am[uo][m] != 0.0) amM[static_cast<std::size_t>(amN++)] = m;
            }
            auto pmAt = [&](std::uint32_t s) {
                double pm = 0.0;
                for (int j = 0; j < fmN; ++j) {
                    const std::size_t m = fmM[static_cast<std::size_t>(j)];
                    pm += v.fmIndex[uo][m] * out[m][s] * mg[m][s];
                }
                return pm;
            };
            double* dst = out[uo];
            switch (*type) {
                case OscType::Va: {
                    const auto wave = static_cast<Wave>(ramps[index(osc(o), OscF::wave)].to);
                    const bool sync = o != 0 && sync1 && ramps[index(osc(o), OscF::sync)].to > 0.5;
                    for (std::uint32_t s = 0; s < n; ++s) {
                        const double pm = pmAt(s);
                        const double inc = v.inc[uo].next();
                        dst[s] =
                            v.va[uo].next(inc, wave, v.width[uo].next(), v.shape[uo].next(), pm);
                        if (sync && wrap[s] > 0.0) v.va[uo].syncTo(wrap[s], inc);
                    }
                    break;
                }
                case OscType::Wavetable: {
                    const Wavetable& table =
                        P.tables->table(static_cast<Table>(ramps[index(osc(o), OscF::table)].to));
                    for (std::uint32_t s = 0; s < n; ++s) {
                        const double pm = pmAt(s);
                        const double inc = v.inc[uo].next();
                        dst[s] = v.wt[uo].next(inc, table, v.position[uo].next(), pm, wtLimit);
                    }
                    break;
                }
                case OscType::Fm: {
                    const auto wave = static_cast<Wave>(ramps[index(osc(o), OscF::wave)].to);
                    for (std::uint32_t s = 0; s < n; ++s) {
                        const double pm = pmAt(s);
                        const double inc = v.inc[uo].next();
                        dst[s] = v.fmo[uo].next(inc, wave, v.fb[uo].next(), pm);
                    }
                    break;
                }
                case OscType::Noise: {
                    const auto color =
                        static_cast<NoiseColor>(ramps[index(osc(o), OscF::color)].to);
                    for (std::uint32_t s = 0; s < n; ++s) {
                        (void)v.inc[uo].next();
                        dst[s] = v.noise[uo].next(color);
                    }
                    break;
                }
                case OscType::Sub: {
                    const auto wave = static_cast<Wave>(ramps[index(osc(o), OscF::wave)].to);
                    for (std::uint32_t s = 0; s < n; ++s) {
                        const double pm = pmAt(s);
                        const double inc = v.inc[uo].next();
                        dst[s] = v.va[uo].next(inc, wave, 0.5, 0.0, pm);
                    }
                    break;
                }
            }
            for (int j = 0; j < amN; ++j) {
                const std::size_t m = amM[static_cast<std::size_t>(j)];
                const double a = v.amDepth[uo][m];
                for (std::uint32_t s = 0; s < n; ++s) dst[s] *= 1.0 - a + a * out[m][s] * mg[m][s];
            }
            if (o == 0)  // what sync reads: osc1's wrap after each of its samples
                for (std::uint32_t s = 0; s < n; ++s) wrap[s] = v.va[0].wrapped();
        }
        for (std::size_t o = 0; o < kOscCount; ++o)
            v.prevOut[o] = P.st.osc[o] ? out[o][n - 1] * mg[o][n - 1] : 0.0;

        // Mixer.
        auto& in1 = sb.in1;
        auto& in2 = sb.in2;
        auto& direct = sb.direct;
        for (int c = 0; c < 2; ++c)
            for (std::uint32_t s = 0; s < n; ++s) in1[c][s] = in2[c][s] = direct[c][s] = 0.0;
        for (std::size_t o = 0; o < kOscCount; ++o) {
            if (!P.st.osc[o]) continue;
            const OscTarget to = P.st.to[o];
            double* a = to == OscTarget::Filter2  ? in2[0]
                        : to == OscTarget::Direct ? direct[0]
                                                  : in1[0];
            double* b = to == OscTarget::Filter2  ? in2[1]
                        : to == OscTarget::Direct ? direct[1]
                                                  : in1[1];
            for (std::uint32_t s = 0; s < n; ++s) {
                const double gl = v.gainL[o].next(), gr = v.gainR[o].next();
                const double l = out[o][s] * gl, r = out[o][s] * gr;
                if (to == OscTarget::None) continue;
                a[s] += l, b[s] += r;
                if (to == OscTarget::Both) in2[0][s] += l, in2[1][s] += r;
            }
        }
        for (std::uint32_t s = 0; s < n; ++s) sb.b1[s] = v.bal1.next(), sb.b2[s] = v.bal2.next();

        // Filters: coefficients as the Lerps give them, then each filter over the buffer.
        auto& co = sb.co;
        for (std::size_t fi = 0; fi < kFilterCount; ++fi) {
            if (!P.st.filter[fi]) continue;
            for (std::uint32_t s = 0; s < n; ++s) {
                co[fi][0][s] = v.g[fi].next(), co[fi][1][s] = v.k[fi].next();
                co[fi][2][s] = v.drive[fi].next(), co[fi][3][s] = v.level[fi].next();
                co[fi][4][s] = v.delay[fi].next(), co[fi][5][s] = v.feedback[fi].next();
                co[fi][6][s] = v.damping[fi].next();
            }
        }
        if (P.st.drive != DriveType::Off)
            for (std::uint32_t s = 0; s < n; ++s) {
                sb.dg[s] = v.driveGain.next(), sb.db[s] = v.driveBias.next();
                sb.dm[s] = v.driveMix.next(), sb.dc[s] = v.driveComp.next();
            }
        for (std::uint32_t s = 0; s < n; ++s) sb.al[s] = v.ampL.next(), sb.ar[s] = v.ampR.next();
    }

    // One filter over the buffers of `count` voices of one program, the
    // recursions interleaved sample by sample so independent voices overlap.
    void filterRunGroup(Voice* const* vs, StageBuf* const* bs, int count, int fi, std::size_t c,
                        const double* const* x, double* const* y, std::uint32_t n) SUPPLE_RT {
        if (count == 1) {
            const double* co[7] = {bs[0]->co[fi][0], bs[0]->co[fi][1], bs[0]->co[fi][2],
                                   bs[0]->co[fi][3], bs[0]->co[fi][4], bs[0]->co[fi][5],
                                   bs[0]->co[fi][6]};
            filterRun(*vs[0], fi, c, x[0], y[0], n, co);
            return;
        }
        const auto u = static_cast<std::size_t>(fi);
        switch (*prog->st.filter[u]) {
            case FilterType::Ladder: {
                const bool db24 =
                    static_cast<Slope>(ramps[index(filter(fi), FilterF::slope)].to) == Slope::Db24;
                for (std::uint32_t s = 0; s < n; ++s)
                    for (int j = 0; j < count; ++j) {
                        const auto& co = bs[j]->co[fi];
                        y[j][s] =
                            vs[j]->ladder[u][c].next(x[j][s], co[0][s], co[1][s], co[2][s], db24) *
                            co[3][s];
                    }
                break;
            }
            case FilterType::Svf: {
                const auto mode = static_cast<SvfMode>(ramps[index(filter(fi), FilterF::mode)].to);
                for (std::uint32_t s = 0; s < n; ++s)
                    for (int j = 0; j < count; ++j) {
                        const auto& co = bs[j]->co[fi];
                        const double d = co[2][s];
                        const double in =
                            d > 1.0001 ? dsp::fastTanh(x[j][s] * d) / std::sqrt(d) : x[j][s];
                        y[j][s] = vs[j]->svf[u][c].next(in, co[0][s], co[1][s], mode) * co[3][s];
                    }
                break;
            }
            default:  // comb and formant: one voice at a time
                for (int j = 0; j < count; ++j) {
                    const double* co[7] = {bs[j]->co[fi][0], bs[j]->co[fi][1], bs[j]->co[fi][2],
                                           bs[j]->co[fi][3], bs[j]->co[fi][4], bs[j]->co[fi][5],
                                           bs[j]->co[fi][6]};
                    filterRun(*vs[j], fi, c, x[j], y[j], n, co);
                }
                break;
        }
    }

    // Stage 2: the filters (routing, balance, direct path) into StageBuf::y.
    void stageFilters(Voice* const* vs, StageBuf* const* bs, int count, std::uint32_t n) SUPPLE_RT {
        const Program& P = *prog;
        const bool hasF1 = P.st.filter[0].has_value(), hasF2 = P.st.filter[1].has_value();
        const bool parallel = P.st.routing == FilterRouting::Parallel;
        for (std::size_t c = 0; c < 2; ++c) {
            Voice* gv[kGroupMax];
            StageBuf* gb[kGroupMax];
            int m = 0;
            for (int j = 0; j < count; ++j)
                if (bs[j]->channels > static_cast<int>(c)) gv[m] = vs[j], gb[m] = bs[j], ++m;
            if (m == 0) break;
            const double* x[kGroupMax];
            double* out[kGroupMax];
            for (int j = 0; j < m; ++j) x[j] = gb[j]->in1[c], out[j] = gb[j]->f1;
            if (hasF1)
                filterRunGroup(gv, gb, m, 0, c, x, out, n);
            else
                for (int j = 0; j < m; ++j) std::copy_n(gb[j]->in1[c], n, gb[j]->f1);
            if (parallel) {
                for (int j = 0; j < m; ++j) x[j] = gb[j]->in2[c], out[j] = gb[j]->f2;
                if (hasF2)
                    filterRunGroup(gv, gb, m, 1, c, x, out, n);
                else
                    for (int j = 0; j < m; ++j) std::copy_n(gb[j]->in2[c], n, gb[j]->f2);
                for (int j = 0; j < m; ++j) {
                    StageBuf& b = *gb[j];
                    for (std::uint32_t s = 0; s < n; ++s)
                        b.y[c][s] = (b.f1[s] * b.b1[s] + b.f2[s] * b.b2[s]) + b.direct[c][s];
                }
            } else {
                for (int j = 0; j < m; ++j) {
                    StageBuf& b = *gb[j];
                    for (std::uint32_t s = 0; s < n; ++s) b.x2[s] = b.in2[c][s] + b.f1[s];
                    x[j] = b.x2, out[j] = b.f2;
                }
                if (hasF2) {
                    filterRunGroup(gv, gb, m, 1, c, x, out, n);
                    for (int j = 0; j < m; ++j) {
                        StageBuf& b = *gb[j];
                        for (std::uint32_t s = 0; s < n; ++s) b.y[c][s] = b.f2[s] + b.direct[c][s];
                    }
                } else {
                    for (int j = 0; j < m; ++j) {
                        StageBuf& b = *gb[j];
                        for (std::uint32_t s = 0; s < n; ++s) b.y[c][s] = b.x2[s] + b.direct[c][s];
                    }
                }
            }
        }
    }

    // Stage 3: drive, then amp (and the steal fade) into the bus.
    void stagePost(Voice& v, std::size_t offset, std::uint32_t n, StageBuf& sb) SUPPLE_RT {
        const Program& P = *prog;
        const auto driveCurve = static_cast<DriveCurve>(static_cast<int>(P.st.drive) - 1);
        if (P.st.drive != DriveType::Off)
            for (int ch = 0; ch < sb.channels; ++ch) {
                double* yc = sb.y[ch];
                for (std::uint32_t s = 0; s < n; ++s)
                    yc[s] +=
                        (drive(yc[s], driveCurve, sb.dg[s], sb.db[s], sb.dc[s]) - yc[s]) * sb.dm[s];
            }
        if (sb.mono) std::copy_n(sb.y[0], n, sb.y[1]);
        const bool fading = v.state == Voice::State::Fading;
        for (std::uint32_t s = 0; s < n; ++s) {
            double gl = sb.al[s], gr = sb.ar[s];
            if (fading) {
                gl *= v.fade;
                gr *= v.fade;
                v.fade = std::max(0.0, v.fade - v.fadeStep);
            }
            busL[offset + s] += static_cast<float>(sb.y[0][s] * gl);
            busR[offset + s] += static_cast<float>(sb.y[1][s] * gr);
        }
    }

    void renderVoiceStaged(Voice& v, std::size_t offset, std::uint32_t n) SUPPLE_RT {
        StageBuf& sb = stages[0];
        stagePre(v, n, sb);
        Voice* vs[1] = {&v};
        StageBuf* bs[1] = {&sb};
        stageFilters(vs, bs, 1, n);
        stagePost(v, offset, n, sb);
    }

    // The unison voices of one note, in lockstep (same start, so the same
    // tick grid): each tick's segment is staged for all of them, and their
    // filters run interleaved.
    void renderGroupSegment(Voice* const* group, int count, std::int64_t a, std::int64_t b,
                            std::int64_t blockStart) SUPPLE_RT {
        const auto os = static_cast<std::uint32_t>(prog->os);
        Voice* vs[kGroupMax];
        StageBuf* bs[kGroupMax];
        std::int64_t t = a;
        while (t < b) {
            const std::uint32_t tickPos = group[0]->tickPos;
            int m = 0;
            for (int j = 0; j < count; ++j) {
                Voice& v = *group[j];
                if (v.state == Voice::State::Free) continue;
                if (tickPos == 0) {
                    voiceTick(v, t);
                    if (v.env[0].idle() && !v.fresh) {
                        v.state = Voice::State::Free;
                        continue;
                    }
                }
                vs[m] = &v;
                bs[m] = &stages[static_cast<std::size_t>(m)];
                ++m;
            }
            const auto n =
                static_cast<std::uint32_t>(std::min<std::int64_t>(b - t, kTick - tickPos));
            for (int j = 0; j < count; ++j) group[j]->tickPos = (tickPos + n) % kTick;
            if (m == 0) break;
            for (int j = 0; j < m; ++j) stagePre(*vs[j], n * os, *bs[j]);
            stageFilters(vs, bs, m, n * os);
            for (int j = 0; j < m; ++j)
                stagePost(*vs[j], static_cast<std::size_t>(t - blockStart) * os, n * os, *bs[j]);
            t += n;
        }
    }

    // Every sounding voice over [a, b): the unison voices of a note together
    // when the program renders staged, the rest one by one.
    void renderVoices(std::int64_t a, std::int64_t b, std::int64_t blockStart) SUPPLE_RT {
        bool done[kVoicePool] = {};
        for (std::size_t i = 0; i < voices.size(); ++i) {
            Voice& v = voices[i];
            if (v.state == Voice::State::Free || done[i]) continue;
            if (prog->staged && v.state == Voice::State::Active) {
                Voice* group[kGroupMax];
                int count = 0;
                for (std::size_t j = i; j < voices.size() && count < kGroupMax; ++j) {
                    Voice& w = voices[j];
                    if (!done[j] && w.state == Voice::State::Active && w.serial == v.serial &&
                        w.tickPos == v.tickPos) {
                        group[count++] = &w;
                        done[j] = true;
                    }
                }
                if (count > 1) {
                    renderGroupSegment(group, count, a, b, blockStart);
                    continue;
                }
            }
            done[i] = true;
            renderVoiceSegment(v, a, b, blockStart);
        }
    }

    // Renders n internal samples of voice v into the bus at `offset`.
    void renderVoice(Voice& v, std::size_t offset, std::uint32_t n) SUPPLE_RT {
        if (prog->staged) {
            renderVoiceStaged(v, offset, n);
            return;
        }
        const Program& P = *prog;
        const bool hasF1 = P.st.filter[0].has_value(), hasF2 = P.st.filter[1].has_value();
        const bool parallel = P.st.routing == FilterRouting::Parallel;
        const auto driveCurve = static_cast<DriveCurve>(static_cast<int>(P.st.drive) - 1);
        std::array<Wave, kOscCount> waves{};
        std::array<NoiseColor, kOscCount> colors{};
        std::array<const Wavetable*, kOscCount> tables{};
        const double wtLimit = wavetableLimit(sr * P.os);
        for (int o = 0; o < kOscCount; ++o) {
            if (!P.st.osc[static_cast<std::size_t>(o)]) continue;
            if (P.tables)
                tables[static_cast<std::size_t>(o)] =
                    &P.tables->table(static_cast<Table>(ramps[index(osc(o), OscF::table)].to));
            waves[static_cast<std::size_t>(o)] =
                static_cast<Wave>(ramps[index(osc(o), OscF::wave)].to);
            colors[static_cast<std::size_t>(o)] =
                static_cast<NoiseColor>(ramps[index(osc(o), OscF::color)].to);
        }
        const bool sync1 = P.st.osc[0].has_value();
        // Centred oscillators make the pre-amp path mono (unison spread and
        // amp pan come after it): run the filters and drive once. Going back to
        // stereo, channel 1 inherits channel 0's filter state.
        bool mono = true;
        for (std::size_t o = 0; o < kOscCount; ++o)
            if (P.st.osc[o] &&
                (v.gainL[o].value != v.gainR[o].value || v.gainL[o].step != v.gainR[o].step))
                mono = false;
        if (!mono && v.monoPath)
            for (std::size_t fi = 0; fi < kFilterCount; ++fi) {
                v.svf[fi][1] = v.svf[fi][0];
                v.ladder[fi][1] = v.ladder[fi][0];
                v.comb[fi][1].copyState(v.comb[fi][0]);
                v.formant[fi][1] = v.formant[fi][0];
            }
        v.monoPath = mono;
        const int channels = mono ? 1 : 2;
        for (std::uint32_t s = 0; s < n; ++s) {
            std::array<double, kOscCount> out{}, mg{};
            for (std::size_t o = 0; o < kOscCount; ++o)
                if (P.st.osc[o]) mg[o] = v.modGain[o].next();
            for (int oi = 0; oi < kOscCount; ++oi) {
                const int o = P.order[static_cast<std::size_t>(oi)];
                const auto uo = static_cast<std::size_t>(o);
                const auto& type = P.st.osc[uo];
                if (!type) continue;
                double pm = 0.0;
                for (std::size_t m = 0; m < kOscCount; ++m)
                    if (v.fmIndex[uo][m] != 0.0)
                        pm += v.fmIndex[uo][m] * (P.delayed[uo][m] ? v.prevOut[m] : out[m] * mg[m]);
                const double inc = v.inc[uo].next();
                double x = 0.0;
                switch (*type) {
                    case OscType::Va:
                        x = v.va[uo].next(inc, waves[uo], v.width[uo].next(), v.shape[uo].next(),
                                          pm);
                        if (o != 0 && sync1 && ramps[index(osc(o), OscF::sync)].to > 0.5 &&
                            v.va[0].wrapped() > 0.0)
                            v.va[uo].syncTo(v.va[0].wrapped(), inc);
                        break;
                    case OscType::Wavetable:
                        x = v.wt[uo].next(inc, *tables[uo], v.position[uo].next(), pm, wtLimit);
                        break;
                    case OscType::Fm:
                        x = v.fmo[uo].next(inc, waves[uo], v.fb[uo].next(), pm);
                        break;
                    case OscType::Noise:
                        x = v.noise[uo].next(colors[uo]);
                        break;
                    case OscType::Sub:
                        x = v.va[uo].next(inc, waves[uo], 0.5, 0.0, pm);
                        break;
                }
                for (std::size_t m = 0; m < kOscCount; ++m)
                    if (P.am[uo][m] != 0.0) {
                        const double a = v.amDepth[uo][m];
                        x *= 1.0 - a + a * (P.delayed[uo][m] ? v.prevOut[m] : out[m] * mg[m]);
                    }
                out[uo] = x;
            }
            for (std::size_t o = 0; o < kOscCount; ++o) v.prevOut[o] = out[o] * mg[o];
            // Mixer: per-oscillator level/pan into the filter inputs.
            double in1[2] = {0.0, 0.0}, in2[2] = {0.0, 0.0}, direct[2] = {0.0, 0.0};
            for (std::size_t o = 0; o < kOscCount; ++o) {
                if (!P.st.osc[o]) continue;
                const double gl = v.gainL[o].next(), gr = v.gainR[o].next();
                const double l = out[o] * gl, r = out[o] * gr;
                switch (P.st.to[o]) {
                    case OscTarget::Filter1:
                        in1[0] += l, in1[1] += r;
                        break;
                    case OscTarget::Filter2:
                        in2[0] += l, in2[1] += r;
                        break;
                    case OscTarget::Both:
                        in1[0] += l, in1[1] += r;
                        in2[0] += l, in2[1] += r;
                        break;
                    case OscTarget::Direct:
                        direct[0] += l, direct[1] += r;
                        break;
                    case OscTarget::None:
                        break;
                }
            }
            const double b1 = v.bal1.next(), b2 = v.bal2.next();
            double y[2];
            for (int ch = 0; ch < channels; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                double f1 = hasF1 ? filterSample(v, 0, ch, in1[c]) : in1[c];
                double res = 0.0;
                if (parallel) {
                    const double f2 = hasF2 ? filterSample(v, 1, ch, in2[c]) : in2[c];
                    res = f1 * b1 + f2 * b2;
                } else {
                    res = hasF2 ? filterSample(v, 1, ch, in2[c] + f1) : in2[c] + f1;
                }
                y[c] = res + direct[c];
            }
            for (std::size_t fi = 0; fi < kFilterCount; ++fi) {
                if (!P.st.filter[fi]) continue;
                v.g[fi].next(), v.k[fi].next(), v.drive[fi].next(), v.level[fi].next();
                v.delay[fi].next(), v.feedback[fi].next(), v.damping[fi].next();
            }
            if (P.st.drive != DriveType::Off) {
                const double dg = v.driveGain.next(), db = v.driveBias.next(),
                             dm = v.driveMix.next();
                const double dc = v.driveComp.next();
                for (int ch = 0; ch < channels; ++ch)
                    y[ch] += (drive(y[ch], driveCurve, dg, db, dc) - y[ch]) * dm;
            }
            if (mono) y[1] = y[0];
            double gl = v.ampL.next(), gr = v.ampR.next();
            if (v.state == Voice::State::Fading) {
                gl *= v.fade;
                gr *= v.fade;
                v.fade = std::max(0.0, v.fade - v.fadeStep);
            }
            busL[offset + s] += static_cast<float>(y[0] * gl);
            busR[offset + s] += static_cast<float>(y[1] * gr);
        }
    }

    void renderVoiceSegment(Voice& v, std::int64_t a, std::int64_t b,
                            std::int64_t blockStart) SUPPLE_RT {
        const auto os = static_cast<std::uint32_t>(prog->os);
        std::int64_t t = a;
        while (t < b && v.state != Voice::State::Free) {
            if (v.tickPos == 0) {
                voiceTick(v, t);
                if (v.env[0].idle() && !v.fresh) {
                    v.state = Voice::State::Free;
                    break;
                }
            }
            const auto n =
                static_cast<std::uint32_t>(std::min<std::int64_t>(b - t, kTick - v.tickPos));
            renderVoice(v, static_cast<std::size_t>(t - blockStart) * os, n * os);
            v.tickPos = (v.tickPos + n) % kTick;
            t += n;
            if (v.state == Voice::State::Fading && v.fade <= 0.0) v.state = Voice::State::Free;
        }
    }

    // --- notes ----------------------------------------------------------------------------------
    int activeNotes() const SUPPLE_RT {
        // Distinct serials among active (not fading) voices.
        int count = 0;
        std::uint64_t seen[Engine::kMaxVoices];
        for (const Voice& v : voices) {
            if (v.state != Voice::State::Active) continue;
            bool dup = false;
            for (int i = 0; i < count; ++i) dup = dup || seen[i] == v.serial;
            if (!dup && count < Engine::kMaxVoices) seen[count++] = v.serial;
        }
        return count;
    }

    void fadeOut(Voice& v) SUPPLE_RT {
        v.state = Voice::State::Fading;
        v.fade = 1.0;
        v.fadeStep = 1.0 / static_cast<double>(kFadeSamples * static_cast<std::uint32_t>(prog->os));
    }

    // Steals the oldest note, preferring released ones.
    void stealOne() SUPPLE_RT {
        std::uint64_t victim = 0;
        bool victimReleased = false;
        for (const Voice& v : voices) {
            if (v.state != Voice::State::Active) continue;
            const bool released = !v.gate && !v.sustained;
            if (victim == 0 || (released && !victimReleased) ||
                (released == victimReleased && v.serial < victim)) {
                victim = v.serial;
                victimReleased = released;
            }
        }
        for (Voice& v : voices)
            if (v.state == Voice::State::Active && v.serial == victim) fadeOut(v);
    }

    int countState(Voice::State s) const SUPPLE_RT {
        int n = 0;
        for (const Voice& v : voices) n += v.state == s;
        return n;
    }

    void startVoice(Voice& v, int key, int channel, std::int32_t noteId, double velocity, int u,
                    int count, std::uint64_t noteSerial, double glideFrom,
                    std::int64_t t) SUPPLE_RT {
        const Program& P = *prog;
        v.state = Voice::State::Active;
        v.key = key;
        v.channel = channel;
        v.noteId = noteId;
        v.velocity = velocity;
        v.releaseVelocity = 0.0;
        v.serial = noteSerial;
        v.unison = u;
        v.unisonCount = count;
        v.gate = true;
        v.sustained = false;
        v.tickPos = 0;
        v.fresh = true;
        v.bend = v.pressure = v.slide = 0.0;
        v.rng.reseed(dsp::mixSeed(P.seed, noteSerial * 8 + static_cast<std::uint64_t>(u)));
        v.random = v.rng.uniform(-1.0, 1.0);
        const Frame& f = frameAt(t);
        for (int o = 0; o < kOscCount; ++o) {
            const auto uo = static_cast<std::size_t>(o);
            const bool reset = static_cast<PhaseMode>(base(index(osc(o), OscF::phase_mode), t)) ==
                               PhaseMode::Reset;
            const double phase =
                reset ? base(index(osc(o), OscF::phase), t) / 360.0 : v.rng.uniform();
            v.va[uo].reset(phase);
            v.fmo[uo].reset(phase);
            v.wt[uo].reset(phase);
            v.noise[uo].seed(v.rng.next());
            v.prevOut[uo] = 0.0;
        }
        for (std::size_t fi = 0; fi < kFilterCount; ++fi)
            for (std::size_t c = 0; c < 2; ++c) {
                v.svf[fi][c].reset();
                v.ladder[fi][c].reset();
                v.comb[fi][c].reset();
                v.formant[fi][c].reset();
            }
        for (int e = 0; e < kEnvCount; ++e) {
            v.env[static_cast<std::size_t>(e)].kill();
            v.env[static_cast<std::size_t>(e)].gate(envParams(v, e, t, f));
        }
        for (int l = 0; l < kLfoCount; ++l) {
            v.lfo[static_cast<std::size_t>(l)].seed(v.rng.next());
            v.lfo[static_cast<std::size_t>(l)].restart(lfoParams(l, &v, t, f));
        }
        for (auto& s : v.step) s.restart();
        v.driftPitch.seed(v.rng.next());
        v.driftPitch.restart({LfoWave::SmoothRandom, 0.3, 0.0, 0.0, false});
        v.driftCutoff.seed(v.rng.next());
        v.driftCutoff.restart({LfoWave::SmoothRandom, 0.23, 0.0, 0.0, false});
        const double glideMs = base(index(kVoice, VoiceF::glide), t);
        if (glideFrom >= 0.0 && glideMs > 0.0) {
            v.glideOffset = glideFrom - key;
            v.glideStep = std::fabs(v.glideOffset) / std::max(1.0, glideMs * 1e-3 * sr);
            if (v.glideOffset < 0.0) v.glideStep = -v.glideStep;
        } else {
            v.glideOffset = 0.0;
        }
    }

    void noteOn(int key, int channel, std::int32_t noteId, double velocity,
                std::int64_t t) SUPPLE_RT {
        const auto mode = static_cast<VoiceMode>(base(index(kVoice, VoiceF::mode), t));
        const bool othersHeld = heldCount > 0;
        // The held-key stack keeps each key once, newest last.
        for (int i = 0; i < heldCount; ++i)
            if (held[static_cast<std::size_t>(i)] == key) {
                for (int j = i; j + 1 < heldCount; ++j)
                    held[static_cast<std::size_t>(j)] = held[static_cast<std::size_t>(j + 1)];
                --heldCount;
                break;
            }
        if (heldCount < kMaxHeld) held[static_cast<std::size_t>(heldCount++)] = key;
        if (!othersHeld) {
            for (int l = 0; l < kLfoCount; ++l)
                if (prog->st.lfoRetrigger[static_cast<std::size_t>(l)] == Retrigger::Mono)
                    monoRestart[static_cast<std::size_t>(l)] = true;
            for (int s = 0; s < kStepCount; ++s)
                if (prog->st.stepRetrigger[static_cast<std::size_t>(s)] == Retrigger::Mono)
                    monoStepRestart[static_cast<std::size_t>(s)] = true;
        }
        const auto glideMode = static_cast<GlideMode>(base(index(kVoice, VoiceF::glide_mode), t));
        const bool glide = lastPitch >= 0.0 && (glideMode == GlideMode::Always || othersHeld);
        if (mode != VoiceMode::Poly && monoSerial != 0) {
            // Retarget the sounding mono note.
            bool found = false;
            for (Voice& v : voices) {
                if (v.state != Voice::State::Active || v.serial != monoSerial) continue;
                found = true;
                retarget(v, key, velocity, othersHeld, mode, glide, t);
            }
            if (found) {
                lastPitch = key;
                return;
            }
        }
        const int unison =
            std::clamp(static_cast<int>(base(index(kVoice, VoiceF::unison), t)), 1, 8);
        const int poly =
            mode == VoiceMode::Poly
                ? std::clamp(static_cast<int>(base(index(kVoice, VoiceF::polyphony), t)), 1, 32)
                : 1;
        while (activeNotes() >= poly && countState(Voice::State::Active) > 0) stealOne();
        while (countState(Voice::State::Active) + unison > Engine::kMaxVoices) stealOne();
        const std::uint64_t noteSerial = ++serial;
        int started = 0;
        for (Voice& v : voices) {
            if (started == unison) break;
            if (v.state != Voice::State::Free) continue;
            startVoice(v, key, channel, noteId, velocity, started, unison, noteSerial,
                       glide ? lastPitch : -1.0, t);
            ++started;
        }
        // Spares exhausted by a burst of steals: take over the fading voices
        // closest to silence (a new note never goes missing).
        while (started < unison) {
            Voice* best = nullptr;
            for (Voice& v : voices)
                if (v.state == Voice::State::Fading && (!best || v.fade < best->fade)) best = &v;
            if (!best) break;
            startVoice(*best, key, channel, noteId, velocity, started, unison, noteSerial,
                       glide ? lastPitch : -1.0, t);
            ++started;
        }
        if (mode != VoiceMode::Poly) monoSerial = noteSerial;
        lastPitch = key;
    }

    // A new key for the sounding mono/legato note. `overlap`: another key was
    // held. Legato never restarts envelopes on overlapping keys; mono restarts
    // those whose retrigger is "always".
    void retarget(Voice& v, int key, double velocity, bool overlap, VoiceMode mode, bool glide,
                  std::int64_t t) SUPPLE_RT {
        const double from = v.key + v.glideOffset;
        v.key = key;
        v.gate = true;
        v.sustained = false;
        const double glideMs = base(index(kVoice, VoiceF::glide), t);
        if (glide && glideMs > 0.0) {
            v.glideOffset = from - key;
            v.glideStep = v.glideOffset / std::max(1.0, glideMs * 1e-3 * sr);
        } else {
            v.glideOffset = 0.0;
        }
        const Frame& f = frameAt(t);
        for (int e = 0; e < kEnvCount; ++e) {
            const bool always = static_cast<EnvRetrigger>(base(index(env(e), EnvF::retrigger),
                                                               t)) == EnvRetrigger::Always;
            if (!overlap || (mode == VoiceMode::Mono && always))
                v.env[static_cast<std::size_t>(e)].gate(envParams(v, e, t, f));
        }
        if (!overlap || mode == VoiceMode::Mono) {
            v.velocity = velocity;
            for (int l = 0; l < kLfoCount; ++l)
                v.lfo[static_cast<std::size_t>(l)].restart(lfoParams(l, &v, t, f));
        }
    }

    void releaseVoice(Voice& v, double velocity) SUPPLE_RT {
        v.gate = false;
        v.releaseVelocity = velocity;
        if (sustainPedal) {
            v.sustained = true;
            return;
        }
        for (auto& e : v.env) e.release();
    }

    void noteOff(int key, int channel, std::int32_t noteId, double velocity,
                 std::int64_t t) SUPPLE_RT {
        // Remove from the held stack.
        for (int i = 0; i < heldCount; ++i)
            if (held[static_cast<std::size_t>(i)] == key) {
                for (int j = i; j + 1 < heldCount; ++j)
                    held[static_cast<std::size_t>(j)] = held[static_cast<std::size_t>(j + 1)];
                --heldCount;
                break;
            }
        const auto mode = static_cast<VoiceMode>(base(index(kVoice, VoiceF::mode), t));
        if (mode != VoiceMode::Poly && monoSerial != 0) {
            for (Voice& v : voices) {
                if (v.state != Voice::State::Active || v.serial != monoSerial || v.key != key)
                    continue;
                if (heldCount > 0)
                    retarget(v, held[static_cast<std::size_t>(heldCount - 1)], v.velocity, true,
                             mode, true, t);
                else
                    releaseVoice(v, velocity);
            }
            if (heldCount > 0) lastPitch = held[static_cast<std::size_t>(heldCount - 1)];
            return;
        }
        for (Voice& v : voices) {
            if (v.state != Voice::State::Active || !v.gate || v.key != key) continue;
            if (noteId >= 0 && v.noteId >= 0 && v.noteId != noteId) continue;
            if (noteId < 0 && channel >= 0 && v.channel != channel) continue;
            releaseVoice(v, velocity);
        }
    }

    void applyEvent(const Event& e, std::int64_t t) SUPPLE_RT {
        const bool mpe = base(index(kVoice, VoiceF::mpe), t) > 0.5;
        switch (e.type) {
            case Event::Type::NoteOn:
                if (e.value <= 0.0)
                    noteOff(e.key, e.channel, e.noteId, 0.0, t);
                else
                    noteOn(e.key, e.channel, e.noteId, std::clamp(e.value, 0.0, 1.0), t);
                break;
            case Event::Type::NoteOff:
                noteOff(e.key, e.channel, e.noteId, e.value, t);
                break;
            case Event::Type::Choke:
                for (Voice& v : voices)
                    if (v.state == Voice::State::Active &&
                        (v.key == e.key || (e.noteId >= 0 && v.noteId == e.noteId)))
                        fadeOut(v);
                break;
            case Event::Type::Param: {
                if (e.index >= ramps.size()) break;
                const ParamSpec& p = tbl[e.index];
                const double v = params::clampPhysical(p, e.value);
                const bool continuous = p.type == params::Type::Float;
                ramps[e.index].set(v, t, continuous ? p.smoothingMs * 1e-3 * sr : 0.0);
                break;
            }
            case Event::Type::Controller: {
                const double v = std::clamp(e.value, 0.0, 1.0);
                const double len = 0.005 * sr;
                switch (e.index) {
                    case 1:
                        modwheel.set(v, t, len);
                        break;
                    case 2:
                        breath.set(v, t, len);
                        break;
                    case 11:
                        expression.set(v, t, len);
                        break;
                    case 74:
                        if (mpe && e.channel != 0) {
                            for (Voice& vv : voices)
                                if (vv.state == Voice::State::Active && vv.channel == e.channel)
                                    vv.slide = v;
                        } else {
                            slide.set(v, t, len);
                        }
                        break;
                    case 64:
                        sustainPedal = v >= 0.5;
                        if (!sustainPedal)
                            for (Voice& vv : voices)
                                if (vv.state == Voice::State::Active && vv.sustained) {
                                    vv.sustained = false;
                                    for (auto& en : vv.env) en.release();
                                }
                        break;
                    case 120:  // All Sound Off: everything stops now, pedal or not
                        for (Voice& vv : voices)
                            if (vv.state == Voice::State::Active) fadeOut(vv);
                        sustainPedal = false;
                        heldCount = 0;
                        monoSerial = 0;
                        break;
                    case 123:  // All Notes Off: releases (sustained notes wait for the pedal)
                        for (Voice& vv : voices)
                            if (vv.state == Voice::State::Active) releaseVoice(vv, 0.0);
                        heldCount = 0;
                        break;
                    default:
                        break;
                }
                break;
            }
            case Event::Type::Pitchbend:
                if (mpe && e.channel != 0) {
                    for (Voice& v : voices)
                        if (v.state == Voice::State::Active && v.channel == e.channel)
                            v.bend = std::clamp(e.value, -1.0, 1.0) * 48.0;
                } else {
                    pitchbend.set(std::clamp(e.value, -1.0, 1.0), t, 0.002 * sr);
                }
                break;
            case Event::Type::Pressure:
                if (mpe && e.channel != 0) {
                    for (Voice& v : voices)
                        if (v.state == Voice::State::Active && v.channel == e.channel)
                            v.pressure = std::clamp(e.value, 0.0, 1.0);
                } else {
                    aftertouch.set(std::clamp(e.value, 0.0, 1.0), t, 0.005 * sr);
                }
                break;
            case Event::Type::PolyPressure:
                for (Voice& v : voices)
                    if (v.state == Voice::State::Active && v.key == e.key &&
                        (e.noteId >= 0 ? v.noteId == e.noteId : v.channel == e.channel))
                        v.pressure = std::clamp(e.value, 0.0, 1.0);
                break;
            case Event::Type::AllNotesOff:
                for (Voice& v : voices)
                    if (v.state == Voice::State::Active) releaseVoice(v, 0.0);
                heldCount = 0;
                break;
        }
    }

    // --- program adoption
    // --------------------------------------------------------------------------
    void adopt(Program* next, Program* previous) SUPPLE_RT {
        const bool first = previous == nullptr;
        for (std::size_t i = 0; i < ramps.size(); ++i) {
            const double v = next->values[i];
            if (first) {
                ramps[i].snap(v);
            } else if (ramps[i].to != v) {
                const ParamSpec& p = tbl[i];
                ramps[i].set(v, now,
                             p.type == params::Type::Float ? p.smoothingMs * 1e-3 * sr : 0.0);
            }
        }
        if (first || next->os != decFactor) {
            // Quality changed: sounding voices restart their DSP at the new
            // rate (envelopes, gate and pitch continue), decimators reset.
            for (Voice& v : voices) {
                if (v.state == Voice::State::Free) continue;
                if (!first && v.state == Voice::State::Fading)  // keep the fade's duration
                    v.fadeStep *= static_cast<double>(decFactor) / static_cast<double>(next->os);
                for (std::size_t fi = 0; fi < static_cast<std::size_t>(kFilterCount); ++fi)
                    for (std::size_t c = 0; c < 2; ++c) {
                        v.svf[fi][c].reset();
                        v.ladder[fi][c].reset();
                        v.comb[fi][c].reset();
                        v.formant[fi][c].reset();
                    }
                v.prevOut.fill(0.0);
                v.fresh = true;
            }
            decL[decIndex(next->os)].reset();
            decR[decIndex(next->os)].reset();
            decFactor = next->os;
        }
        if (!first) {
            for (Voice& v : voices) {
                if (v.state == Voice::State::Free) continue;
                for (int o = 0; o < kOscCount; ++o)
                    if (!sameOsc(*previous, *next, o)) {
                        v.va[static_cast<std::size_t>(o)].reset(v.rng.uniform());
                        v.fmo[static_cast<std::size_t>(o)].reset(v.rng.uniform());
                        v.wt[static_cast<std::size_t>(o)].reset(v.rng.uniform());
                    }
                for (int fi = 0; fi < kFilterCount; ++fi)
                    if (!sameFilter(*previous, *next, fi))
                        for (std::size_t c = 0; c < 2; ++c) {
                            v.svf[static_cast<std::size_t>(fi)][c].reset();
                            v.ladder[static_cast<std::size_t>(fi)][c].reset();
                            v.comb[static_cast<std::size_t>(fi)][c].reset();
                            v.formant[static_cast<std::size_t>(fi)][c].reset();
                        }
                v.mod.fill(0.0);
            }
            // Restart LFOs whose scope changed; mod slots were renumbered.
            for (auto& f : frames) f.mod.fill(0.0);
        }
    }
};

// --- Engine ------------------------------------------------------------------------------------

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

void Engine::prepare(double sampleRate, std::uint32_t maxBlock) {
    Impl& m = *impl_;
    sampleRate_ = sampleRate;
    m.sr = sampleRate;
    m.maxBlock = maxBlock;
    m.ramps.assign(paramCount(), Ramp{});
    for (std::size_t i = 0; i < m.ramps.size(); ++i) m.ramps[i].snap(table()[i].def);
    m.frames.assign(maxBlock / kTick + 3, Frame{});
    const std::size_t busSize = static_cast<std::size_t>(maxBlock) * 4;
    m.busL.assign(busSize, 0.0f);
    m.busR.assign(busSize, 0.0f);
    m.outL.assign(maxBlock, 0.0f);
    m.outR.assign(maxBlock, 0.0f);
    // Comb delay lines: up to 1/20 s at 4x, per voice, filter slot and channel.
    const std::size_t combLen = static_cast<std::size_t>(sampleRate * 4.0 / 20.0) + 8;
    m.combMemory.assign(combLen * kVoicePool * kFilterCount * 2, 0.0f);
    std::size_t at = 0;
    for (Voice& v : m.voices)
        for (auto& slot : v.comb)
            for (auto& c : slot) {
                c.attach(std::span<float>(m.combMemory.data() + at, combLen));
                at += combLen;
            }
    for (int os : {1, 2, 4}) {
        m.decL[Impl::decIndex(os)].setFactor(os);
        m.decR[Impl::decIndex(os)].setFactor(os);
    }
    m.decFactor = 0;
    m.fx.prepare(sampleRate);
    m.now = 0;
    // Re-adopt the current program on the next block (values snap to it).
    m.prog = nullptr;
}

void Engine::setPatch(const Patch& patch) { impl_->handoff.publish(compileProgram(patch)); }

void Engine::collect() { impl_->handoff.collect(); }

void Engine::reset() {
    Impl& m = *impl_;
    for (Voice& v : m.voices) v.state = Voice::State::Free;
    m.heldCount = 0;
    m.monoSerial = 0;
    m.lastPitch = -1.0;
    m.serial = 0;
    m.sustainPedal = false;
    for (auto& d : m.decL) d.reset();
    for (auto& d : m.decR) d.reset();
    m.fx.reset();
    for (auto* r : {&m.modwheel, &m.breath, &m.expression, &m.slide, &m.aftertouch}) r->snap(0.0);
    m.pitchbend.snap(0.0);
    for (std::size_t l = 0; l < kLfoCount; ++l) {
        m.glfo[l].seed(dsp::mixSeed(m.prog ? m.prog->seed : 0, 1000 + l));
        m.monoRestart[l] = true;
    }
    for (std::size_t s = 0; s < kStepCount; ++s) m.monoStepRestart[s] = true;
    for (auto& f : m.frames) f = Frame{};
    m.now = 0;
    m.quiet = 0;
}

int Engine::activeVoices() const {
    int n = 0;
    for (const Voice& v : impl_->voices) n += v.state != Voice::State::Free;
    return n;
}

bool Engine::silent() const {
    return activeVoices() == 0 && impl_->quiet > static_cast<std::uint64_t>(impl_->sr * 0.1);
}

void Engine::process(std::span<const Event> events, const Transport& transport, float* left,
                     float* right, std::uint32_t frames) {
    Impl& m = *impl_;
    assert(frames <= m.maxBlock);
#ifdef ORRERY_HAS_MXCSR
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040);  // flush-to-zero, denormals-are-zero
#endif
    Program* previous = m.prog;
    Program* next = m.handoff.acquire();
    if (next != previous) {
        m.prog = next;
        if (next) {
            if (!previous) {
                // First program: seed the global modulators.
                for (std::size_t l = 0; l < kLfoCount; ++l) {
                    m.glfo[l].seed(dsp::mixSeed(next->seed, 1000 + l));
                    m.monoRestart[l] = true;
                }
                for (std::size_t s = 0; s < kStepCount; ++s) m.monoStepRestart[s] = true;
            }
            m.adopt(next, previous);
        }
    }
    if (!m.prog) {
        std::fill_n(left, frames, 0.0f);
        std::fill_n(right, frames, 0.0f);
#ifdef ORRERY_HAS_MXCSR
        _mm_setcsr(csr);
#endif
        return;
    }
    m.tempo = std::isfinite(transport.tempo) ? std::clamp(transport.tempo, 20.0, 999.0) : 120.0;
    const Program& P = *m.prog;
    const auto os = static_cast<std::uint32_t>(P.os);
    const std::int64_t B = m.now;
    const std::int64_t tick = static_cast<std::int64_t>(kTick);
    m.firstTick = (B + tick - 1) / tick;
    // frames[0] carries the last frame computed before this block (copied at
    // the end of the previous one).

    auto renderSegment = [&](std::int64_t a, std::int64_t b) {
        // 1. Global ticks in [a, b).
        for (std::int64_t g = (a + tick - 1) / tick * tick; g < b; g += tick) m.globalTick(g);
        // 2. Voices into the oversampled bus.
        const auto off = static_cast<std::size_t>(a - B) * os;
        const auto len = static_cast<std::size_t>(b - a) * os;
        std::fill_n(m.busL.begin() + static_cast<std::ptrdiff_t>(off), len, 0.0f);
        std::fill_n(m.busR.begin() + static_cast<std::ptrdiff_t>(off), len, 0.0f);
        m.renderVoices(a, b, B);
        // 3. Decimate to the base rate.
        const auto boff = static_cast<std::size_t>(a - B);
        const auto n = static_cast<std::uint32_t>(b - a);
        m.decL[Impl::decIndex(P.os)].process(m.busL.data() + off, m.outL.data() + boff, n);
        m.decR[Impl::decIndex(P.os)].process(m.busR.data() + off, m.outR.data() + boff, n);
        // 4. FX and output, with parameter updates on the absolute tick grid.
        std::int64_t t = a;
        while (t < b) {
            if (t % tick == 0) {
                const Frame& f = m.frameAt(t);
                for (FxKind kind : P.st.chain) {
                    const Slot s = static_cast<Slot>(kChorus + static_cast<int>(kind));
                    const std::uint16_t count = fieldCount(slotInfo(s).block);
                    for (std::uint16_t fi = 0; fi < count; ++fi)
                        m.fxValues[fi] = m.geff(index(s, fi), t, f);
                    m.fx.set(kind, std::span<const double>(m.fxValues.data(), count), m.tempo);
                }
                const double target = dsp::levelGain(m.geff(index(kOutput, OutputF::level), t, f));
                m.outGain.set(m.outGain.value, target, static_cast<double>(kTick));
            }
            const std::int64_t end = std::min(b, (t / tick + 1) * tick);
            const auto o = static_cast<std::size_t>(t - B);
            const auto cn = static_cast<std::uint32_t>(end - t);
            for (FxKind kind : P.st.chain)
                m.fx.process(kind, m.outL.data() + o, m.outR.data() + o, cn);
            for (std::uint32_t i = 0; i < cn; ++i) {
                const double gl = m.outGain.next();
                left[o + i] =
                    dsp::safetyClip(static_cast<float>(static_cast<double>(m.outL[o + i]) * gl));
                right[o + i] =
                    dsp::safetyClip(static_cast<float>(static_cast<double>(m.outR[o + i]) * gl));
                const bool q = std::fabs(left[o + i]) < 1e-6f && std::fabs(right[o + i]) < 1e-6f;
                m.quiet = q ? m.quiet + 1 : 0;
            }
            t = end;
        }
    };

    std::size_t e = 0;
    std::uint32_t pos = 0;
    while (pos < frames) {
        std::uint32_t end = frames;
        if (e < events.size()) end = std::clamp<std::uint32_t>(events[e].time, pos, frames);
        if (end > pos) renderSegment(B + pos, B + end);
        pos = end;
        while (e < events.size() && std::min(events[e].time, frames) <= pos) {
            m.applyEvent(events[e], B + std::min<std::int64_t>(events[e].time, frames));
            ++e;
        }
    }
    while (e < events.size()) m.applyEvent(events[e++], B + frames);
    // Carry the last computed frame into slot 0 for the next block.
    if (m.lastFrameTick >= m.firstTick) {
        const auto k = static_cast<std::size_t>(m.lastFrameTick - m.firstTick + 1);
        m.frames[0] = m.frames[k];
    }
    m.now += frames;
#ifdef ORRERY_HAS_MXCSR
    _mm_setcsr(csr);
#endif
}

}  // namespace supple::orrery
