// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Marram: Quire's synthesizer.
//
// Marram grass holds a dune together by its roots; under the wind its blades
// sing. Each note is one voice, and a voice takes this road:
//
//   Oscillators  four, each off, or a classic analog wave (saw, square,
//                pulse, triangle, sine; hard sync to Osc 1), a wavetable
//                (seven original tables, made here from harmonic recipes),
//                a sine operator for FM (with feedback), noise, or a sub an
//                octave or two down. Any one may frequency-modulate another.
//                Unison stacks up to eight detuned copies of each pitched
//                oscillator across the stereo field, with a shared filter.
//   Filters      two, in series or in parallel: a transistor ladder
//                (12 or 24 dB, saturating), a state-variable filter, a tuned
//                comb, or a bank of vowel formants. Each oscillator goes to
//                one, both, or past them.
//   Drive        soft, tape, fold or hard, after the filters.
//   Amp          its own envelope, with velocity and pan.
//
// The mod envelope (Env 2) moves each filter's cutoff, the pitch and the
// depth of FM by amounts set here; velocity and the key move the cutoff too.
// Drift wanders each voice's pitch and cutoff a little, the way old
// circuits do.
//
// Everything a voice does runs oversampled (2x at Normal quality, 4x at
// High), summed, and brought down once. Modulation is worked out every 16
// samples of a voice's life and glided in between, so a render is the same
// at any block size; every random choice (free-running phases, drift,
// noise) comes from the order of the notes, so a render is the same every
// time.

constexpr int kVoices = 16;
constexpr int kUnison = 8;
constexpr int kTick = 16;  // frames between a voice's control updates
constexpr double kVoiceGain = 0.5;

// Oscillator types and the filters', as their choices number them.
constexpr int kOff = 0;
constexpr int kAnalog = 1;
constexpr int kWavetable = 2;
constexpr int kFm = 3;
constexpr int kNoise = 4;
constexpr int kSub = 5;
constexpr int kLadder = 1;
constexpr int kSvf = 2;
constexpr int kComb = 3;
constexpr int kFormant = 4;

// The wavetables: kTables tables of kFrames frames, each frame kLevels
// band-limited copies (level L holds at most 256 >> L harmonics in
// max(256, 2048 >> L) samples, plus one that repeats the first).
constexpr int kTables = 7;
constexpr int kFrames = 16;
constexpr int kLevels = 9;
constexpr int kTop = 256;
constexpr int kFft = 2048;
constexpr int kPerFrame = 5129;  // 2049 + 1025 + 513 + 6 * 257

// --- small pieces ------------------------------------------------------------------------

double wrap01(double x) { return x - floor(x); }

// The polyBLEP residual of a unit step at phase 0, and its integral (for a
// change of slope), t the phase and dt its step per sample.
double blep(double t, double dt) {
    if (t < dt) {
        const double x = t / dt;
        return x + x - x * x - 1.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}
double blamp(double t, double dt) {
    if (t < dt) {
        const double x = t / dt - 1.0;
        return -x * x * x / 3.0;
    }
    if (t > 1.0 - dt) {
        const double x = (t - 1.0) / dt + 1.0;
        return x * x * x / 3.0;
    }
    return 0.0;
}

double pulseWave(double t, double dt, double w) {
    const double naive = t < w ? 1.0 : -1.0;
    return naive + blep(t, dt) - blep(wrap01(t - w), dt);
}

// Saw, square, pulse, triangle, sine: band-limited where they break.
double analogWave(int wave, double t, double dt, double width) {
    if (wave == 0) return 2.0 * t - 1.0 - blep(t, dt);
    if (wave == 1) return pulseWave(t, dt, 0.5);
    if (wave == 2) return pulseWave(t, dt, width);
    if (wave == 3) {
        const double naive = t < 0.5 ? 4.0 * t - 1.0 : 3.0 - 4.0 * t;
        return naive + 8.0 * dt * (blamp(t, dt) - blamp(wrap01(t - 0.5), dt));
    }
    return sin(twoPi * t);
}

// tanh, near enough, and exactly +-1 beyond 4.97.
double fastTanh(double x) {
    if (x > 4.97) return 1.0;
    if (x < -4.97) return -1.0;
    const double x2 = x * x;
    return x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2))) /
           (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

// An envelope's bend: x 0..1 along a segment; c 0 straight, toward 1 an
// exponential that starts fast and settles slowly when falling.
double rise(double x, double c) {
    if (c == 0.0) return x;
    const double k = c * 6.0;
    return (exp(k * x) - 1.0) / (exp(k) - 1.0);
}
double fall(double x, double c) { return 1.0 - rise(1.0 - x, c); }

// The drive's curves (soft, tape, fold, hard), each with its resting point
// taken off so a bias adds no offset.
double tapeCurve(double v) { return v >= 0.0 ? fastTanh(v * 1.2) / 1.2 : fastTanh(v * 0.8) / 0.8; }
double driveCurve(int type, double u, double bias) {
    if (type == 1) return fastTanh(u + bias) - fastTanh(bias);
    if (type == 2) return tapeCurve(u + bias) - tapeCurve(bias);
    if (type == 3) return sin(0.5 * pi * (u + bias)) - sin(0.5 * pi * bias);
    return clamp(u + bias, -1.0, 1.0) - clamp(bias, -1.0, 1.0);
}

// --- the voice ---------------------------------------------------------------------------

// A DAHDSR without the D and H: attack, decay, sustain, release, each segment
// exactly as long as its time and bent: the attack straight, the decay and
// release exponential. It starts from wherever it is, so retriggering never
// clicks.
struct Env {
    int stage = 0;  // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
    double level = 0.0;
    double start = 0.0;
    double x = 0.0;

    void gate() {
        start = level;
        stage = 1;
        x = 0.0;
    }
    void release() {
        if (stage == 0 || stage == 4) return;
        start = level;
        stage = 4;
        x = 0.0;
    }
    void kill() {
        stage = 0;
        level = 0.0;
        start = 0.0;
        x = 0.0;
    }
    // On by dt samples, times in samples; the level at the end.
    double advance(double dt, double a, double d, double s, double r) {
        int guard = 0;
        while (dt > 0.0 && guard < 6) {
            ++guard;
            if (stage == 0) {
                level = 0.0;
                return level;
            }
            if (stage == 3) {
                level = s;
                return level;
            }
            const double length = stage == 1 ? a : stage == 2 ? d : r;
            if (length <= 0.5) {
                x = 1.0;
            } else {
                const double left = (1.0 - x) * length;
                if (dt < left) {
                    x += dt / length;
                    dt = 0.0;
                } else {
                    dt -= left;
                    x = 1.0;
                }
            }
            if (stage == 1)
                level = start + (1.0 - start) * x;
            else if (stage == 2)
                level = 1.0 + (s - 1.0) * fall(x, 0.5);
            else
                level = start * (1.0 - fall(x, 0.5));
            if (x >= 1.0) {
                x = 0.0;
                stage = stage == 1 ? 2 : stage == 2 ? 3 : 0;
                if (stage == 2) start = 1.0;
                if (stage == 0) level = 0.0;
            }
        }
        return level;
    }
};

// One channel of one filter slot: every type's state, so switching types
// never reads another's.
struct Filter {
    Array<double, 4> stage;  // the ladder's
    double ic1 = 0.0;        // the state-variable filter's
    double ic2 = 0.0;
    Array<double, 6> band;  // the formant bank's three
    DelayLine comb;
    double combLp = 0.0;

    // The other channel's state, as it goes from one channel to two (the
    // comb's line, never fed while there was one, stays empty).
    void follow(const Filter& o) {
        stage = o.stage;
        ic1 = o.ic1;
        ic2 = o.ic2;
        band = o.band;
        combLp = 0.0;
    }

    void clear() {
        stage.fill(0.0);
        ic1 = 0.0;
        ic2 = 0.0;
        band.fill(0.0);
        combLp = 0.0;
    }

    // A transistor ladder: four one-poles with their feedback solved at once
    // (no delay), the input saturating. g the integrators' gain, k 0..4 (it
    // sings near 4), drive a gain.
    double ladder(double x, double g, double k, double drive, bool steep) {
        const double G = g / (1.0 + g);
        const double B = 1.0 / (1.0 + g);
        const double S =
            G * G * G * B * stage[0] + G * G * B * stage[1] + G * B * stage[2] + B * stage[3];
        const double G4 = G * G * G * G;
        const double y4 = (G4 * x * drive + S) / (1.0 + k * G4);
        double u = fastTanh(x * drive - k * y4);
        double half = 0.0;
        for (int i = 0; i < 4; ++i) {
            const double v = (u - stage[i]) * G;
            const double y = v + stage[i];
            stage[i] = y + v;
            u = y;
            if (i == 1) half = y;
        }
        return (steep ? u : half) * (1.0 + 0.5 * k) / max(1.0, drive * 0.5 + 0.5);
    }

    // A state-variable filter (trapezoidal), k = 1/Q. Mode: low, band, high
    // pass, notch.
    double svf(double x, double g, double k, int mode) {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        if (mode == 0) return v2;
        if (mode == 1) return v1;
        const double high = x - k * v1 - v2;
        if (mode == 2) return high;
        return v2 + high;
    }

    // Band b of the formant bank, a band pass with 0 dB at its peak.
    double formantBand(int b, double x, double g, double k) {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double s1 = band[b * 2];
        const double s2 = band[b * 2 + 1];
        const double v3 = x - s2;
        const double v1 = a1 * s1 + a2 * v3;
        const double v2 = s2 + a2 * s1 + a3 * v3;
        band[b * 2] = 2.0 * v1 - s1;
        band[b * 2 + 1] = 2.0 * v2 - s2;
        return v1 * k;
    }

    // A feedback comb, delay samples long, darkening in its loop.
    double combFilter(double x, double delay, double feedback, double damping) {
        const double tap = comb.read(delay - 1.0);
        combLp += (tap - combLp) * (1.0 - damping * 0.95);
        const double y = x + feedback * combLp;
        comb.push(y);
        return y * (1.0 - abs(feedback) * 0.5);
    }
};

// A filter slot's settings for the next tick, and the glides toward them.
struct FilterNow {
    double g = 0.0;
    double gStep = 0.0;
    double k = 0.0;
    double kStep = 0.0;
    double delay = 1.0;
    double delayStep = 0.0;
    double drive = 1.0;
    double feedback = 0.0;
    double damping = 0.0;
    Array<double, 3> fg;  // the formant bands' gains and widths
    Array<double, 3> fk;
};

struct Voice {
    // Per oscillator and unison copy, at o * 8 + u.
    Array<double, 32> phase;
    Array<double, 32> inc;  // cycles per (oversampled) sample, gliding
    Array<double, 32> incStep;
    Array<double, 32> out;  // the copy's last sample, for FM
    Array<double, 32> fb1;  // an FM operator's last two outputs
    Array<double, 32> fb2;
    Array<double, 32> panL;
    Array<double, 32> panR;
    Array<double, 32> wander;  // each copy's own drift, a standard normal
    Array<double, 8> wrapped;  // where Osc 1's copies wrapped, for sync; -1 not

    // Per oscillator, as of the last tick.
    Array<int, 4> type;
    Array<int, 4> copies;
    Array<double, 4> gain;
    Array<double, 4> gainStep;
    Array<double, 4> fm;  // the depth, in cycles
    Array<int, 4> from;
    Array<double, 4> noise;  // this frame's noise sample
    Array<double, 4> lastPink;
    Array<int, 4> level;  // a wavetable's band level
    Array<int, 4> base0;  // and where its frames start
    Array<int, 4> base1;
    Array<int, 4> size;
    Array<double, 4> blend;
    Array<double, 4> mult;
    Array<Noise, 4> noises;

    Array<Filter, 4> filter;  // filter 1 left, right, filter 2 left, right
    Array<FilterNow, 2> now;

    Env amp;
    Env mod;
    double ampL = 0.0;
    double ampLStep = 0.0;
    double ampR = 0.0;
    double ampRStep = 0.0;
    double velocity = 0.0;
    bool sounding = false;
    bool fresh = false;
    bool done = false;
    bool narrow = true;
    bool wasCentred = true;
    bool ending = false;  // the amp's last target was silence
    int tickLeft = 0;
    double lastL = 0.0;
    double lastR = 0.0;
    // Drift: two slow random walks, pitch and cutoff.
    double driftAt = 0.0;
    double pitchFrom = 0.0;
    double pitchTo = 0.0;
    double cutFrom = 0.0;
    double cutTo = 0.0;
};

// An oscillator's settings, from its parameters.
struct OscSet {
    int type = 0;
    double gain = 0.0;
    double pan = 0.0;
    double tune = 0.0;  // semitones
    int wave = 0;
    double width = 0.5;
    bool sync = false;
    int table = 0;
    double position = 0.0;
    double ratio = 1.0;
    double feedback = 0.0;
    int color = 0;
    int octave = -1;
    double fm = 0.0;
    int from = 0;
    int to = 0;       // filter 1, filter 2, both, direct
    bool toA = true;  // and so: into filter 1's input, filter 2's, or past them
    bool toB = false;
    bool direct = false;
    bool retrigger = false;
};

struct FilterSet {
    int type = 0;
    double cutoff = 1000.0;
    double resonance = 0.0;
    bool steep = true;
    int mode = 0;
    double feedback = 0.0;
    double damping = 0.0;
    double vowel = 0.0;
    double shift = 0.0;
    double keytrack = 0.0;
    double env = 0.0;
    double velocity = 0.0;
    double drive = 1.0;
};

struct Marram : Instrument {
    static constexpr Info info = {
        .name = "Marram",
        .id = "com.suppleaudio.marram",
        .vendor = "Supple Audio",
        .description =
            "Synthesizer: four oscillators (analog, wavetable, FM, noise, sub) with unison, two "
            "filters (ladder, state-variable, comb, formant), drive and two envelopes.",
        .voices = kVoices};

    // --- Voice ----------------------------------------------------------------------------

    Param mode = choice("Mode", {"Poly", "Mono", "Legato"}, 0)
                     .withKey("voice.mode")
                     .in("Voice")
                     .help(
                         "Poly plays chords; Mono one note at a time, each restarting the "
                         "envelopes; Legato one at a time, gliding without restarting them.");
    Param glide = milliseconds("Glide", 0, 5000, 0)
                      .withKey("voice.glide")
                      .in("Voice")
                      .help("How long the pitch takes to slide to a new note.");
    Param bendRange = semitones("Bend Range", 0, 48, 2)
                          .withKey("voice.bend_range")
                          .in("Voice")
                          .help("How far the pitch wheel bends, each way.");
    Param unison =
        integer("Unison", 1, kUnison, 1)
            .withKey("voice.unison")
            .in("Voice")
            .help(
                "Detuned copies of each pitched oscillator per note: 3 thickens, 7 is a supersaw.");
    Param detune = cents("Unison Detune", 0, 100, 12)
                       .withKey("voice.unison_detune")
                       .in("Voice")
                       .activeAbove(unison, 1)
                       .help("How far the outermost copies are detuned, each way.");
    Param spread = percent("Unison Spread", 60)
                       .withKey("voice.unison_spread")
                       .in("Voice")
                       .activeAbove(unison, 1)
                       .help("How wide the copies spread across the stereo field.");
    Param drift = percent("Drift", 0)
                      .withKey("voice.drift")
                      .in("Voice")
                      .help(
                          "Slow random wander of each voice's pitch (up to 5 cents, and each "
                          "oscillator its own) and cutoff, like old analog circuits.");
    Param quality =
        choice("Quality", {"Eco", "Normal", "High"}, 1)
            .withKey("voice.quality")
            .in("Voice")
            .restarts()
            .help("Oversampling: Eco none, Normal 2x, High 4x for heavy FM, sync and drive.");

    // --- Oscillators --------------------------------------------------------------------

    Param o1Type = choice("Osc 1 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 1)
                       .withKey("osc1.type")
                       .in("Osc 1")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o1Level = decibels("Osc 1 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc1.level")
                        .in("Osc 1")
                        .activeUnless(o1Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o1Pan = percent("Osc 1 Pan", 0, -100, 100)
                      .withKey("osc1.pan")
                      .in("Osc 1")
                      .activeUnless(o1Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o1Coarse = integer("Osc 1 Coarse", -48, 48, 0)
                         .withKey("osc1.coarse")
                         .in("Osc 1")
                         .activeUnless(o1Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o1Fine =
        cents("Osc 1 Fine", -100, 100, 0)
            .withKey("osc1.fine")
            .in("Osc 1")
            .activeUnless(o1Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o1Wave = choice("Osc 1 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc1.wave")
                       .in("Osc 1")
                       .activeWhen(o1Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o1Width =
        percent("Osc 1 Width", 25, 1, 99)
            .withKey("osc1.width")
            .in("Osc 1")
            .activeWhen(o1Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o1Table =
        choice("Osc 1 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc1.table")
            .in("Osc 1")
            .activeWhen(o1Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a coarsening digital ramp, or a comb through the harmonics.");
    Param o1Position = percent("Osc 1 Position", 0)
                           .withKey("osc1.position")
                           .in("Osc 1")
                           .activeWhen(o1Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o1Ratio = continuous("Osc 1 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc1.ratio")
                        .in("Osc 1")
                        .activeWhen(o1Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o1Feedback =
        percent("Osc 1 Feedback", 0)
            .withKey("osc1.feedback")
            .in("Osc 1")
            .activeWhen(o1Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o1Color =
        choice("Osc 1 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc1.color")
            .in("Osc 1")
            .activeWhen(o1Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o1Octave = integer("Osc 1 Octave", -2, -1, -1)
                         .withKey("osc1.octave")
                         .in("Osc 1")
                         .activeWhen(o1Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o1Fm = continuous("Osc 1 FM", 0, 10, 0)
                     .withKey("osc1.fm")
                     .in("Osc 1")
                     .activeWhen(o1Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o1FmFrom = choice("Osc 1 FM From", {"Osc 2", "Osc 3", "Osc 4"}, 0)
                         .keys({"osc2", "osc3", "osc4"})
                         .withKey("osc1.fm_from")
                         .in("Osc 1")
                         .activeAbove(o1Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o1To = choice("Osc 1 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc1.to")
                     .in("Osc 1")
                     .activeUnless(o1Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o1Retrigger = toggle("Osc 1 Retrigger", false)
                            .withKey("osc1.retrigger")
                            .in("Osc 1")
                            .activeUnless(o1Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o2Type = choice("Osc 2 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc2.type")
                       .in("Osc 2")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o2Level = decibels("Osc 2 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc2.level")
                        .in("Osc 2")
                        .activeUnless(o2Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o2Pan = percent("Osc 2 Pan", 0, -100, 100)
                      .withKey("osc2.pan")
                      .in("Osc 2")
                      .activeUnless(o2Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o2Coarse = integer("Osc 2 Coarse", -48, 48, 0)
                         .withKey("osc2.coarse")
                         .in("Osc 2")
                         .activeUnless(o2Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o2Fine =
        cents("Osc 2 Fine", -100, 100, 0)
            .withKey("osc2.fine")
            .in("Osc 2")
            .activeUnless(o2Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o2Wave = choice("Osc 2 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc2.wave")
                       .in("Osc 2")
                       .activeWhen(o2Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o2Width =
        percent("Osc 2 Width", 25, 1, 99)
            .withKey("osc2.width")
            .in("Osc 2")
            .activeWhen(o2Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o2Sync = toggle("Osc 2 Sync", false)
                       .withKey("osc2.sync")
                       .in("Osc 2")
                       .activeWhen(o2Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o2Table =
        choice("Osc 2 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc2.table")
            .in("Osc 2")
            .activeWhen(o2Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a coarsening digital ramp, or a comb through the harmonics.");
    Param o2Position = percent("Osc 2 Position", 0)
                           .withKey("osc2.position")
                           .in("Osc 2")
                           .activeWhen(o2Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o2Ratio = continuous("Osc 2 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc2.ratio")
                        .in("Osc 2")
                        .activeWhen(o2Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o2Feedback =
        percent("Osc 2 Feedback", 0)
            .withKey("osc2.feedback")
            .in("Osc 2")
            .activeWhen(o2Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o2Color =
        choice("Osc 2 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc2.color")
            .in("Osc 2")
            .activeWhen(o2Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o2Octave = integer("Osc 2 Octave", -2, -1, -1)
                         .withKey("osc2.octave")
                         .in("Osc 2")
                         .activeWhen(o2Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o2Fm = continuous("Osc 2 FM", 0, 10, 0)
                     .withKey("osc2.fm")
                     .in("Osc 2")
                     .activeWhen(o2Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o2FmFrom = choice("Osc 2 FM From", {"Osc 1", "Osc 3", "Osc 4"}, 0)
                         .keys({"osc1", "osc3", "osc4"})
                         .withKey("osc2.fm_from")
                         .in("Osc 2")
                         .activeAbove(o2Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o2To = choice("Osc 2 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc2.to")
                     .in("Osc 2")
                     .activeUnless(o2Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o2Retrigger = toggle("Osc 2 Retrigger", false)
                            .withKey("osc2.retrigger")
                            .in("Osc 2")
                            .activeUnless(o2Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o3Type = choice("Osc 3 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc3.type")
                       .in("Osc 3")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o3Level = decibels("Osc 3 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc3.level")
                        .in("Osc 3")
                        .activeUnless(o3Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o3Pan = percent("Osc 3 Pan", 0, -100, 100)
                      .withKey("osc3.pan")
                      .in("Osc 3")
                      .activeUnless(o3Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o3Coarse = integer("Osc 3 Coarse", -48, 48, 0)
                         .withKey("osc3.coarse")
                         .in("Osc 3")
                         .activeUnless(o3Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o3Fine =
        cents("Osc 3 Fine", -100, 100, 0)
            .withKey("osc3.fine")
            .in("Osc 3")
            .activeUnless(o3Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o3Wave = choice("Osc 3 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc3.wave")
                       .in("Osc 3")
                       .activeWhen(o3Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o3Width =
        percent("Osc 3 Width", 25, 1, 99)
            .withKey("osc3.width")
            .in("Osc 3")
            .activeWhen(o3Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o3Sync = toggle("Osc 3 Sync", false)
                       .withKey("osc3.sync")
                       .in("Osc 3")
                       .activeWhen(o3Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o3Table =
        choice("Osc 3 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc3.table")
            .in("Osc 3")
            .activeWhen(o3Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a coarsening digital ramp, or a comb through the harmonics.");
    Param o3Position = percent("Osc 3 Position", 0)
                           .withKey("osc3.position")
                           .in("Osc 3")
                           .activeWhen(o3Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o3Ratio = continuous("Osc 3 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc3.ratio")
                        .in("Osc 3")
                        .activeWhen(o3Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o3Feedback =
        percent("Osc 3 Feedback", 0)
            .withKey("osc3.feedback")
            .in("Osc 3")
            .activeWhen(o3Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o3Color =
        choice("Osc 3 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc3.color")
            .in("Osc 3")
            .activeWhen(o3Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o3Octave = integer("Osc 3 Octave", -2, -1, -1)
                         .withKey("osc3.octave")
                         .in("Osc 3")
                         .activeWhen(o3Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o3Fm = continuous("Osc 3 FM", 0, 10, 0)
                     .withKey("osc3.fm")
                     .in("Osc 3")
                     .activeWhen(o3Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o3FmFrom = choice("Osc 3 FM From", {"Osc 1", "Osc 2", "Osc 4"}, 0)
                         .keys({"osc1", "osc2", "osc4"})
                         .withKey("osc3.fm_from")
                         .in("Osc 3")
                         .activeAbove(o3Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o3To = choice("Osc 3 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc3.to")
                     .in("Osc 3")
                     .activeUnless(o3Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o3Retrigger = toggle("Osc 3 Retrigger", false)
                            .withKey("osc3.retrigger")
                            .in("Osc 3")
                            .activeUnless(o3Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    Param o4Type = choice("Osc 4 Type", {"Off", "Analog", "Wavetable", "FM", "Noise", "Sub"}, 0)
                       .withKey("osc4.type")
                       .in("Osc 4")
                       .help(
                           "What the oscillator is: a classic wave, a sweepable table, a sine "
                           "operator for FM, noise, or a sub an octave or two down.");
    Param o4Level = decibels("Osc 4 Level", -60, 6, -6)
                        .offAtMin()
                        .withKey("osc4.level")
                        .in("Osc 4")
                        .activeUnless(o4Type, {"off"})
                        .help(
                            "Its level into the filters. At the minimum it is silent but can still "
                            "modulate another oscillator.");
    Param o4Pan = percent("Osc 4 Pan", 0, -100, 100)
                      .withKey("osc4.pan")
                      .in("Osc 4")
                      .activeUnless(o4Type, {"off"})
                      .help("Where it sits, left (-100 %) to right (+100 %).");
    Param o4Coarse = integer("Osc 4 Coarse", -48, 48, 0)
                         .withKey("osc4.coarse")
                         .in("Osc 4")
                         .activeUnless(o4Type, {"off", "noise", "sub"})
                         .help("Its pitch from the note, in whole semitones.");
    Param o4Fine =
        cents("Osc 4 Fine", -100, 100, 0)
            .withKey("osc4.fine")
            .in("Osc 4")
            .activeUnless(o4Type, {"off", "noise", "sub"})
            .help("Its pitch from the note, in cents: a few apart thicken two oscillators.");
    Param o4Wave = choice("Osc 4 Wave", {"Saw", "Square", "Pulse", "Triangle", "Sine"}, 0)
                       .withKey("osc4.wave")
                       .in("Osc 4")
                       .activeWhen(o4Type, {"analog", "sub"})
                       .help("The waveform, anti-aliased.");
    Param o4Width =
        percent("Osc 4 Width", 25, 1, 99)
            .withKey("osc4.width")
            .in("Osc 4")
            .activeWhen(o4Wave, {"pulse"})
            .help("The pulse's width: 50 % is a square, narrower is thinner and nasal.");
    Param o4Sync = toggle("Osc 4 Sync", false)
                       .withKey("osc4.sync")
                       .in("Osc 4")
                       .activeWhen(o4Type, {"analog"})
                       .help(
                           "Hard sync: restart its cycle with Osc 1's, so tuning it up sweeps a "
                           "tearing formant.");
    Param o4Table =
        choice("Osc 4 Table",
               {"Harmonic Ramp", "PWM", "Formant", "Bell", "Organ", "Digital", "Spectral Comb"}, 0)
            .withKey("osc4.table")
            .in("Osc 4")
            .activeWhen(o4Type, {"wavetable"})
            .help(
                "The table: a saw rolling off to a sine, a narrowing pulse, sung vowels, a bell, "
                "organ drawbars, a coarsening digital ramp, or a comb through the harmonics.");
    Param o4Position = percent("Osc 4 Position", 0)
                           .withKey("osc4.position")
                           .in("Osc 4")
                           .activeWhen(o4Type, {"wavetable"})
                           .help("Where in the table it plays, start to end.");
    Param o4Ratio = continuous("Osc 4 Ratio", 0.25, 16, 1)
                        .curved(Curve::Log)
                        .withKey("osc4.ratio")
                        .in("Osc 4")
                        .activeWhen(o4Type, {"fm"})
                        .help(
                            "Its frequency as a multiple of the note's: whole numbers sound "
                            "harmonic, others metallic.");
    Param o4Feedback =
        percent("Osc 4 Feedback", 0)
            .withKey("osc4.feedback")
            .in("Osc 4")
            .activeWhen(o4Type, {"fm"})
            .help("The operator modulating itself: a sine turns toward a saw, then noise.");
    Param o4Color =
        choice("Osc 4 Color", {"White", "Pink", "Brown", "Blue"}, 0)
            .withKey("osc4.color")
            .in("Osc 4")
            .activeWhen(o4Type, {"noise"})
            .help("The noise's spectrum: white is flat, pink and brown darker, blue brighter.");
    Param o4Octave = integer("Osc 4 Octave", -2, -1, -1)
                         .withKey("osc4.octave")
                         .in("Osc 4")
                         .activeWhen(o4Type, {"sub"})
                         .help("How many octaves under the note the sub plays.");
    Param o4Fm = continuous("Osc 4 FM", 0, 10, 0)
                     .withKey("osc4.fm")
                     .in("Osc 4")
                     .activeWhen(o4Type, {"analog", "wavetable", "fm"})
                     .help(
                         "Frequency modulation by another oscillator: the peak phase swing in "
                         "radians (the index). 1 to 3 is bright, higher harsh.");
    Param o4FmFrom = choice("Osc 4 FM From", {"Osc 1", "Osc 2", "Osc 3"}, 0)
                         .keys({"osc1", "osc2", "osc3"})
                         .withKey("osc4.fm_from")
                         .in("Osc 4")
                         .activeAbove(o4Fm, 0)
                         .help(
                             "Which oscillator modulates it. Turn that one's level down to hear "
                             "only the result.");
    Param o4To = choice("Osc 4 To", {"Filter 1", "Filter 2", "Both", "Direct"}, 0)
                     .keys({"filter1", "filter2", "both", "direct"})
                     .withKey("osc4.to")
                     .in("Osc 4")
                     .activeUnless(o4Type, {"off"})
                     .help(
                         "Where it goes: a filter, both (in parallel routing; filter 1 in serial), "
                         "or past the filters.");
    Param o4Retrigger = toggle("Osc 4 Retrigger", false)
                            .withKey("osc4.retrigger")
                            .in("Osc 4")
                            .activeUnless(o4Type, {"off", "noise"})
                            .help(
                                "Start each note at the same point of the cycle, for an attack "
                                "that is the same every time. Off, it runs free.");

    // --- Filters -------------------------------------------------------------------------

    Param f1Type = choice("Filter 1 Type", {"Off", "Ladder", "SVF", "Comb", "Formant"}, 1)
                       .withKey("filter1.type")
                       .in("Filter 1")
                       .help(
                           "A warm transistor ladder, a clean state-variable filter, a tuned comb, "
                           "or a bank of vowel formants.");
    Param f1Cutoff = frequency("Filter 1 Cutoff", 20, 20000, 2000)
                         .withKey("filter1.cutoff")
                         .in("Filter 1")
                         .activeWhen(f1Type, {"ladder", "svf", "comb"})
                         .help(
                             "The corner frequency (a comb's pitch). Key tracking, the envelope "
                             "and velocity move it from here.");
    Param f1Resonance = percent("Filter 1 Resonance", 10)
                            .withKey("filter1.resonance")
                            .in("Filter 1")
                            .activeWhen(f1Type, {"ladder", "svf"})
                            .help(
                                "The peak at the cutoff: 40 % a bump, 70 % clearly resonant, near "
                                "100 % it rings and the ladder sings.");
    Param f1Slope = choice("Filter 1 Slope", {"12 dB/oct", "24 dB/oct"}, 1)
                        .keys({"db12", "db24"})
                        .withKey("filter1.slope")
                        .in("Filter 1")
                        .activeWhen(f1Type, {"ladder"})
                        .help("How steeply the ladder cuts above the cutoff.");
    Param f1Mode = choice("Filter 1 Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0)
                       .keys({"lowpass", "bandpass", "highpass", "notch"})
                       .withKey("filter1.mode")
                       .in("Filter 1")
                       .activeWhen(f1Type, {"svf"})
                       .help("What the state-variable filter keeps.");
    Param f1Feedback = percent("Filter 1 Feedback", 50, -99, 99)
                           .withKey("filter1.feedback")
                           .in("Filter 1")
                           .activeWhen(f1Type, {"comb"})
                           .help(
                               "How long the comb rings; negative feedback keeps the odd "
                               "harmonics, an octave down and hollow.");
    Param f1Damping = percent("Filter 1 Damping", 20)
                          .withKey("filter1.damping")
                          .in("Filter 1")
                          .activeWhen(f1Type, {"comb"})
                          .help("How much the comb darkens as it rings, like a plucked string.");
    Param f1Vowel =
        continuous("Filter 1 Vowel", 0, 4, 0)
            .withKey("filter1.vowel")
            .in("Filter 1")
            .activeWhen(f1Type, {"formant"})
            .help("The vowel, from a (0) through e, i and o to u (4), blending between.");
    Param f1Shift = semitones("Filter 1 Shift", -12, 12, 0)
                        .withKey("filter1.shift")
                        .in("Filter 1")
                        .activeWhen(f1Type, {"formant"})
                        .help("Moves the formants up or down: a smaller or larger throat.");
    Param f1Keytrack = percent("Filter 1 Keytrack", 0, 0, 200)
                           .withKey("filter1.keytrack")
                           .in("Filter 1")
                           .activeUnless(f1Type, {"off"})
                           .help(
                               "How far the cutoff follows the note from middle C: 100 % moves it "
                               "an octave per octave.");
    Param f1Env =
        octaves("Filter 1 Env", -8, 8, 2)
            .withKey("filter1.env")
            .in("Filter 1")
            .activeUnless(f1Type, {"off"})
            .help("How far the mod envelope (Env 2) moves the cutoff at its peak, in octaves.");
    Param f1Velocity =
        octaves("Filter 1 Velocity", 0, 4, 0)
            .withKey("filter1.velocity")
            .in("Filter 1")
            .activeUnless(f1Type, {"off"})
            .help("How far the hardest notes open the cutoff, in octaves; softer ones less.");
    Param f1Drive = decibels("Filter 1 Drive", 0, 24, 0)
                        .withKey("filter1.drive")
                        .in("Filter 1")
                        .activeUnless(f1Type, {"off"})
                        .help(
                            "Gain into the filter: the ladder saturates and thickens, the others "
                            "simply get louder.");

    Param f2Type = choice("Filter 2 Type", {"Off", "Ladder", "SVF", "Comb", "Formant"}, 0)
                       .withKey("filter2.type")
                       .in("Filter 2")
                       .help(
                           "A warm transistor ladder, a clean state-variable filter, a tuned comb, "
                           "or a bank of vowel formants.");
    Param f2Cutoff = frequency("Filter 2 Cutoff", 20, 20000, 1000)
                         .withKey("filter2.cutoff")
                         .in("Filter 2")
                         .activeWhen(f2Type, {"ladder", "svf", "comb"})
                         .help(
                             "The corner frequency (a comb's pitch). Key tracking, the envelope "
                             "and velocity move it from here.");
    Param f2Resonance = percent("Filter 2 Resonance", 10)
                            .withKey("filter2.resonance")
                            .in("Filter 2")
                            .activeWhen(f2Type, {"ladder", "svf"})
                            .help(
                                "The peak at the cutoff: 40 % a bump, 70 % clearly resonant, near "
                                "100 % it rings and the ladder sings.");
    Param f2Slope = choice("Filter 2 Slope", {"12 dB/oct", "24 dB/oct"}, 1)
                        .keys({"db12", "db24"})
                        .withKey("filter2.slope")
                        .in("Filter 2")
                        .activeWhen(f2Type, {"ladder"})
                        .help("How steeply the ladder cuts above the cutoff.");
    Param f2Mode = choice("Filter 2 Mode", {"Low Pass", "Band Pass", "High Pass", "Notch"}, 0)
                       .keys({"lowpass", "bandpass", "highpass", "notch"})
                       .withKey("filter2.mode")
                       .in("Filter 2")
                       .activeWhen(f2Type, {"svf"})
                       .help("What the state-variable filter keeps.");
    Param f2Feedback = percent("Filter 2 Feedback", 50, -99, 99)
                           .withKey("filter2.feedback")
                           .in("Filter 2")
                           .activeWhen(f2Type, {"comb"})
                           .help(
                               "How long the comb rings; negative feedback keeps the odd "
                               "harmonics, an octave down and hollow.");
    Param f2Damping = percent("Filter 2 Damping", 20)
                          .withKey("filter2.damping")
                          .in("Filter 2")
                          .activeWhen(f2Type, {"comb"})
                          .help("How much the comb darkens as it rings, like a plucked string.");
    Param f2Vowel =
        continuous("Filter 2 Vowel", 0, 4, 0)
            .withKey("filter2.vowel")
            .in("Filter 2")
            .activeWhen(f2Type, {"formant"})
            .help("The vowel, from a (0) through e, i and o to u (4), blending between.");
    Param f2Shift = semitones("Filter 2 Shift", -12, 12, 0)
                        .withKey("filter2.shift")
                        .in("Filter 2")
                        .activeWhen(f2Type, {"formant"})
                        .help("Moves the formants up or down: a smaller or larger throat.");
    Param f2Keytrack = percent("Filter 2 Keytrack", 0, 0, 200)
                           .withKey("filter2.keytrack")
                           .in("Filter 2")
                           .activeUnless(f2Type, {"off"})
                           .help(
                               "How far the cutoff follows the note from middle C: 100 % moves it "
                               "an octave per octave.");
    Param f2Env =
        octaves("Filter 2 Env", -8, 8, 0)
            .withKey("filter2.env")
            .in("Filter 2")
            .activeUnless(f2Type, {"off"})
            .help("How far the mod envelope (Env 2) moves the cutoff at its peak, in octaves.");
    Param f2Velocity =
        octaves("Filter 2 Velocity", 0, 4, 0)
            .withKey("filter2.velocity")
            .in("Filter 2")
            .activeUnless(f2Type, {"off"})
            .help("How far the hardest notes open the cutoff, in octaves; softer ones less.");
    Param f2Drive = decibels("Filter 2 Drive", 0, 24, 0)
                        .withKey("filter2.drive")
                        .in("Filter 2")
                        .activeUnless(f2Type, {"off"})
                        .help(
                            "Gain into the filter: the ladder saturates and thickens, the others "
                            "simply get louder.");

    Param routing = choice("Filter Routing", {"Serial", "Parallel"}, 0)
                        .withKey("filters.routing")
                        .in("Filter 2")
                        .activeUnless(f1Type, {"off"})
                        .activeUnless(f2Type, {"off"})
                        .help(
                            "Serial runs filter 1 into filter 2; parallel runs them side by side "
                            "and mixes them.");

    // --- Drive, amp, envelopes ------------------------------------------------------------

    Param driveType = choice("Drive", {"Off", "Soft", "Tape", "Fold", "Hard"}, 0)
                          .withKey("drive.type")
                          .in("Drive")
                          .help(
                              "Saturation after the filters: soft and tape round off, fold turns "
                              "the wave back on itself, hard clips.");
    Param driveAmount = decibels("Drive Amount", 0, 36, 6)
                            .withKey("drive.amount")
                            .in("Drive")
                            .activeUnless(driveType, {"off"})
                            .help("Gain into the curve; the level out is held about even.");
    Param driveBias = percent("Drive Bias", 0, -100, 100)
                          .withKey("drive.bias")
                          .in("Drive")
                          .activeUnless(driveType, {"off"})
                          .help("Leans the curve to one side, for even harmonics.");
    Param driveMix = percent("Drive Mix", 100)
                         .withKey("drive.mix")
                         .in("Drive")
                         .activeUnless(driveType, {"off"})
                         .help("How much of the driven sound is heard against the clean.");

    Param ampAttack = milliseconds("Amp Attack", 0, 30000, 2)
                          .withKey("amp.attack")
                          .in("Amp")
                          .help("How long a note takes to rise.");
    Param ampDecay = milliseconds("Amp Decay", 1, 30000, 600)
                         .withKey("amp.decay")
                         .in("Amp")
                         .help("How long it then takes to fall to the sustain level.");
    Param ampSustain = percent("Amp Sustain", 80)
                           .withKey("amp.sustain")
                           .in("Amp")
                           .help("The level held while the key is down.");
    Param ampRelease = milliseconds("Amp Release", 1, 30000, 250)
                           .withKey("amp.release")
                           .in("Amp")
                           .help("How long the note takes to die away once the key is up.");
    Param ampVelocity =
        percent("Amp Velocity", 50)
            .withKey("amp.velocity")
            .in("Amp")
            .help("How much softer quiet notes play: at 100 % the softest is 40 dB down.");
    Param ampPan = percent("Pan", 0, -100, 100)
                       .withKey("amp.pan")
                       .in("Amp")
                       .help("Where the whole sound sits, left to right.");

    Param envAttack = milliseconds("Env 2 Attack", 0, 30000, 1)
                          .withKey("env2.attack")
                          .in("Env 2")
                          .help(
                              "The mod envelope's rise. It moves the cutoffs, the pitch and FM by "
                              "the amounts set for each.");
    Param envDecay = milliseconds("Env 2 Decay", 1, 30000, 400)
                         .withKey("env2.decay")
                         .in("Env 2")
                         .help("Its fall to the sustain level.");
    Param envSustain = percent("Env 2 Sustain", 20)
                           .withKey("env2.sustain")
                           .in("Env 2")
                           .help("Its level while the key is down.");
    Param envRelease = milliseconds("Env 2 Release", 1, 30000, 300)
                           .withKey("env2.release")
                           .in("Env 2")
                           .help("Its fall once the key is up.");
    Param envPitch = semitones("Env 2 Pitch", -48, 48, 0)
                         .withKey("env2.pitch")
                         .in("Env 2")
                         .help(
                             "How far it bends every oscillator's pitch at its peak: a drum's or a "
                             "zap's sweep.");
    Param envFm = percent("Env 2 FM", 0)
                      .withKey("env2.fm")
                      .in("Env 2")
                      .help(
                          "How much of each oscillator's FM depth follows it: at 100 % the depth "
                          "rises and falls with the envelope, as an electric piano's bark does.");

    Param output =
        decibels("Output", -36, 12, 0)
            .smoothed(20)
            .in("Amp")
            .help("The level out. A gentle limit above -1 dBFS keeps chords from clipping.");

    // --- State ------------------------------------------------------------------------------

    Array<Voice, kVoices> voice;
    Array<Random, kVoices> random;  // each voice's, seeded by the note it plays
    Array<OscSet, 4> osc;
    Array<FilterSet, 2> filt;
    Array<int, 2> combWas;  // each filter's type, as update() last saw it
    Array<double, kUnison> detuneRatio;
    Array<double, kUnison> spreadAt;
    int copies = 1;
    double copyGain = 1.0;
    int factor = 2;
    double rate = 96000.0;  // the voices' (oversampled) rate
    double limit = 0.2;     // the highest harmonic a wavetable keeps, in cycles per sample
    Buffer<double> mixL;
    Buffer<double> mixR;
    Array<Oversampler, 2> over;
    double restL = 0.0;  // what stolen voices were playing, fading out
    double restR = 0.0;
    double restFade = 0.99;
    int notes = 0;
    int quiet = 0;
    bool tablesMade = false;

    // The wavetables, and what makes them.
    Buffer<float> tables;
    Buffer<double> re;
    Buffer<double> im;
    Buffer<double> twiddleC;
    Buffer<double> twiddleS;
    Array<double, 257> amp;  // a frame's harmonics, amplitude and phase
    Array<double, 257> phs;
    Array<int, kLevels> levelSize;
    Array<int, kLevels> levelAt;

    // --- Hooks ------------------------------------------------------------------------------

    void prepare() {
        factor = quality.index() == 0 ? 1 : quality.index() == 1 ? 2 : 4;
        rate = sampleRate() * factor;
        limit = min(0.5, 20000.0 / rate);
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(factor);
            over[c].clear();
        }
        setLatency(over[0].latency());
        mixL.resize(maxBlockSize() * 4);
        mixR.resize(maxBlockSize() * 4);
        const int longest = ceilToInt(rate / 25.0) + 4;
        for (Voice& s : voice) {
            for (int f = 0; f < 4; ++f) s.filter[f].comb.prepare(longest);
            silence(s);
        }
        combWas.fill(kComb);  // prepared lines are empty
        restL = 0.0;
        restR = 0.0;
        restFade = exp(-1.0 / (0.003 * rate));
        notes = 0;
        quiet = 0;
        if (!tablesMade) makeTables();
        tablesMade = true;
    }

    void update() {
        loadOsc(osc[0], 0, o1Type, o1Level, o1Pan, o1Coarse, o1Fine, o1Wave, o1Width, o1Table,
                o1Position, o1Ratio, o1Feedback, o1Color, o1Octave, o1Fm, o1FmFrom, o1To,
                o1Retrigger);
        loadOsc(osc[1], 1, o2Type, o2Level, o2Pan, o2Coarse, o2Fine, o2Wave, o2Width, o2Table,
                o2Position, o2Ratio, o2Feedback, o2Color, o2Octave, o2Fm, o2FmFrom, o2To,
                o2Retrigger);
        loadOsc(osc[2], 2, o3Type, o3Level, o3Pan, o3Coarse, o3Fine, o3Wave, o3Width, o3Table,
                o3Position, o3Ratio, o3Feedback, o3Color, o3Octave, o3Fm, o3FmFrom, o3To,
                o3Retrigger);
        loadOsc(osc[3], 3, o4Type, o4Level, o4Pan, o4Coarse, o4Fine, o4Wave, o4Width, o4Table,
                o4Position, o4Ratio, o4Feedback, o4Color, o4Octave, o4Fm, o4FmFrom, o4To,
                o4Retrigger);
        osc[1].sync = o2Sync.on();
        osc[2].sync = o3Sync.on();
        osc[3].sync = o4Sync.on();
        loadFilter(filt[0], f1Type, f1Cutoff, f1Resonance, f1Slope, f1Mode, f1Feedback, f1Damping,
                   f1Vowel, f1Shift, f1Keytrack, f1Env, f1Velocity, f1Drive);
        loadFilter(filt[1], f2Type, f2Cutoff, f2Resonance, f2Slope, f2Mode, f2Feedback, f2Damping,
                   f2Vowel, f2Shift, f2Keytrack, f2Env, f2Velocity, f2Drive);
        // A filter that becomes a comb starts from an empty line, not from
        // what a note long gone left in it.
        for (int f = 0; f < 2; ++f) {
            if (filt[f].type == kComb && combWas[f] != kComb)
                for (Voice& s : voice) {
                    s.filter[f * 2].comb.clear();
                    s.filter[f * 2 + 1].comb.clear();
                }
            combWas[f] = filt[f].type;
        }
        // Routing: in series an oscillator sent to both enters filter 1.
        const bool parallel = routing.index() == 1;
        for (int o = 0; o < 4; ++o) {
            OscSet& c = osc[o];
            c.toA = c.to == 0 || c.to == 2;
            c.toB = c.to == 1 || (c.to == 2 && parallel);
            c.direct = c.to == 3;
        }
        copies = unison.index();
        copyGain = 1.0 / sqrt(copies);
        for (int u = 0; u < kUnison; ++u) {
            const double at = copies > 1 ? 2.0 * u / (copies - 1) - 1.0 : 0.0;
            detuneRatio[u] = exp2(detune * at / 1200.0);
            spreadAt[u] = spread * at;
        }
        setVoiceMode(mode.index() == 0   ? VoiceMode::Poly
                     : mode.index() == 1 ? VoiceMode::Mono
                                         : VoiceMode::Legato);
        setGlide(glide);
        setBendRange(bendRange);
    }

    void loadOsc(OscSet& c, int self, const Param& type, const Param& level, const Param& pan,
                 const Param& coarse, const Param& fine, const Param& wave, const Param& width,
                 const Param& table, const Param& position, const Param& ratio,
                 const Param& feedback, const Param& color, const Param& octave, const Param& fm,
                 const Param& fmFrom, const Param& to, const Param& retrigger) {
        c.type = type.index();
        c.gain = level.gain();
        c.pan = pan;
        c.tune = coarse.index() + fine / 100.0;
        c.wave = wave.index();
        c.width = width;
        c.table = table.index();
        c.position = position;
        c.ratio = ratio;
        c.feedback = feedback;
        c.color = color.index();
        c.octave = octave.index();
        c.fm = fm;
        c.from = fmFrom.index() < self ? fmFrom.index() : fmFrom.index() + 1;
        c.to = to.index();
        c.retrigger = retrigger.on();
    }

    void loadFilter(FilterSet& f, const Param& type, const Param& cutoff, const Param& resonance,
                    const Param& slope, const Param& svfMode, const Param& feedback,
                    const Param& damping, const Param& vowel, const Param& shift,
                    const Param& keytrack, const Param& env, const Param& velocity,
                    const Param& drive) {
        f.type = type.index();
        f.cutoff = cutoff;
        f.resonance = resonance;
        f.steep = slope.index() == 1;
        f.mode = svfMode.index();
        f.feedback = feedback;
        f.damping = damping;
        f.vowel = vowel;
        f.shift = shift;
        f.keytrack = keytrack;
        f.env = env;
        f.velocity = velocity;
        f.drive = dbToGain(drive);
    }

    // --- Notes ------------------------------------------------------------------------------

    void startVoice(int v, bool retrigger) {
        Voice& s = voice[v];
        Random& dice = random[v];
        ++notes;
        dice.seed(notes * 7919 + 17);
        s.velocity = voiceVelocity(v);
        if (!s.sounding) {
            for (int f = 0; f < 4; ++f) {
                s.filter[f].clear();
                if (filt[f / 2].type == kComb) s.filter[f].comb.clear();
            }
            for (int x = 0; x < 32; ++x) {
                s.phase[x] = osc[x / 8].retrigger ? 0.0 : dice.next();
                s.out[x] = 0.0;
                s.fb1[x] = 0.0;
                s.fb2[x] = 0.0;
                s.wander[x] = dice.gaussian();
            }
            for (int o = 0; o < 4; ++o) {
                s.noises[o].seed(dice.below(1000000000));
                s.lastPink[o] = 0.0;
                s.noise[o] = 0.0;
                s.gain[o] = 0.0;
                s.gainStep[o] = 0.0;
            }
            s.wrapped.fill(-1.0);
            s.amp.kill();
            s.mod.kill();
            s.ampL = 0.0;
            s.ampR = 0.0;
            s.ampLStep = 0.0;
            s.ampRStep = 0.0;
            s.driftAt = dice.next();
            s.pitchFrom = dice.bipolar();
            s.pitchTo = dice.bipolar();
            s.cutFrom = dice.bipolar();
            s.cutTo = dice.bipolar();
            s.narrow = true;
            s.wasCentred = true;
            s.ending = false;
            s.fresh = true;
            s.tickLeft = 0;
        }
        if (retrigger || !s.sounding) {
            s.amp.gate();
            s.mod.gate();
        }
        s.sounding = true;
        s.done = false;
    }

    void releaseVoice(int v) {
        voice[v].amp.release();
        voice[v].mod.release();
    }

    // Stolen or choked: it stops now, and what it was playing fades out over
    // a few milliseconds instead of stepping to silence.
    void stopVoice(int v) {
        Voice& s = voice[v];
        if (s.sounding) {
            restL += s.lastL;
            restR += s.lastR;
        }
        silence(s);
    }

    void silence(Voice& s) {
        s.sounding = false;
        s.done = false;
        s.amp.kill();
        s.mod.kill();
        s.ampL = 0.0;
        s.ampR = 0.0;
        s.ampLStep = 0.0;
        s.ampRStep = 0.0;
        s.lastL = 0.0;
        s.lastR = 0.0;
        s.tickLeft = 0;
        for (int f = 0; f < 4; ++f) s.filter[f].clear();
    }

    // --- Control: every kTick frames of a voice -------------------------------------------

    double ms(double t) { return t * 0.001 * sampleRate(); }

    void control(int v, int i) {
        Voice& s = voice[v];
        Random& dice = random[v];
        const double steps = kTick * factor;
        const bool snap = s.fresh;
        s.fresh = false;

        const double ampEnv =
            s.amp.advance(kTick, ms(ampAttack), ms(ampDecay), ampSustain, ms(ampRelease));
        const double modEnv =
            s.mod.advance(kTick, ms(envAttack), ms(envDecay), envSustain, ms(envRelease));
        if (s.ending && s.amp.stage == 0 && !snap) {
            s.done = true;
            return;
        }

        // Drift: a new random target about every 3 seconds, eased toward.
        s.driftAt += kTick / sampleRate() * 0.3;
        if (s.driftAt >= 1.0) {
            s.driftAt -= 1.0;
            s.pitchFrom = s.pitchTo;
            s.pitchTo = dice.bipolar();
            s.cutFrom = s.cutTo;
            s.cutTo = dice.bipolar();
        }
        const double ease = s.driftAt * s.driftAt * (3.0 - 2.0 * s.driftAt);
        const double driftPitch = drift * 0.05 * lerp(s.pitchFrom, s.pitchTo, ease);  // semitones
        const double driftCut = drift * 0.043 * lerp(s.cutFrom, s.cutTo, ease);       // octaves

        const double pitch = voicePitch(v, i) + driftPitch + envPitch * modEnv;
        const double fmFollow = 1.0 - envFm + envFm * modEnv;
        bool centred = true;
        for (int o = 0; o < 4; ++o) {
            const OscSet& c = osc[o];
            s.type[o] = c.type;
            if (c.type == kOff) continue;
            const bool stacked = c.type == kAnalog || c.type == kWavetable || c.type == kFm;
            const int n = stacked ? copies : 1;
            s.copies[o] = n;
            double hz = 0.0;
            if (c.type == kSub)
                hz = mtof(pitch + 12.0 * c.octave);
            else if (c.type == kFm)
                hz = mtof(pitch + c.tune) * c.ratio;
            else
                hz = mtof(pitch + c.tune);
            s.mult[o] = 1.0;
            if (c.type == kWavetable) s.mult[o] = c.table == 3 ? 8.0 : c.table == 4 ? 2.0 : 1.0;
            for (int u = 0; u < n; ++u) {
                const int x = o * 8 + u;
                const double ratio =
                    (stacked ? detuneRatio[u] : 1.0) * exp2(drift * s.wander[x] * 0.002);
                const double target = min(hz * ratio / rate, 0.45) / s.mult[o];
                if (snap) {
                    s.inc[x] = target;
                    s.incStep[x] = 0.0;
                } else {
                    s.incStep[x] = (target - s.inc[x]) / steps;
                }
                const double p = clamp(c.pan + (stacked ? spreadAt[u] : 0.0), -1.0, 1.0);
                if (p != 0.0) centred = false;
                const double a = (p + 1.0) * 0.25 * pi;
                s.panL[x] = cos(a) * sqrt(2.0);
                s.panR[x] = sin(a) * sqrt(2.0);
            }
            const double g = c.gain * (stacked ? copyGain : 1.0);
            if (snap) {
                s.gain[o] = g;
                s.gainStep[o] = 0.0;
            } else {
                s.gainStep[o] = (g - s.gain[o]) / steps;
            }
            s.fm[o] = c.type == kNoise || c.type == kSub || osc[c.from].type == kOff
                          ? 0.0
                          : c.fm * fmFollow / twoPi;
            s.from[o] = c.from;
            if (c.type == kWavetable) table(s, o, c, s.inc[o * 8]);
        }

        // Filters.
        const double keyOct = (pitch - 60.0) / 12.0;
        for (int f = 0; f < 2; ++f) {
            const FilterSet& c = filt[f];
            if (c.type == kOff) continue;
            FilterNow& n = s.now[f];
            const double octs =
                c.keytrack * keyOct + c.env * modEnv + c.velocity * s.velocity + driftCut;
            const double hz = clamp(c.cutoff * exp2(octs), 10.0, min(22000.0, 0.45 * rate));
            const double g = tan(pi * hz / rate);
            const double k = c.type == kLadder
                                 ? 4.0 * c.resonance
                                 : max(0.005, 2.0 * (1.0 - c.resonance) * (1.0 - c.resonance));
            const double delay = clamp(rate / max(hz, 25.0), 2.0, rate / 25.0);
            if (snap) {
                n.g = g;
                n.gStep = 0.0;
                n.k = k;
                n.kStep = 0.0;
                n.delay = delay;
                n.delayStep = 0.0;
            } else {
                n.gStep = (g - n.g) / steps;
                n.kStep = (k - n.k) / steps;
                n.delayStep = (delay - n.delay) / steps;
            }
            n.drive = c.drive;
            n.feedback = c.feedback;
            n.damping = c.damping;
            if (c.type == kFormant) formants(n, c.vowel, c.shift + 12.0 * keyOct * c.keytrack);
        }

        // Stereo: a voice whose oscillators all sit in the centre filters one
        // channel and copies it.
        const bool narrow = centred && s.wasCentred;
        if (s.narrow && !narrow) {
            s.filter[1].follow(s.filter[0]);
            s.filter[3].follow(s.filter[2]);
        }
        s.narrow = narrow;
        s.wasCentred = centred;

        // Amp: the envelope, velocity, the note's own volume and pan.
        const double velDb = -(1.0 - s.velocity) * 40.0 * ampVelocity;
        const double level = ampEnv * dbToGain(velDb) * voiceVolume(v) * kVoiceGain;
        const double p = clamp(ampPan + (voicePan(v) - 0.5) * 2.0, -1.0, 1.0);
        const double a = (p + 1.0) * 0.25 * pi;
        const double l = level * cos(a) * sqrt(2.0);
        const double r = level * sin(a) * sqrt(2.0);
        if (snap) {
            s.ampL = 0.0;
            s.ampR = 0.0;
        }
        s.ampLStep = (l - s.ampL) / steps;
        s.ampRStep = (r - s.ampR) / steps;
        s.ending = level == 0.0;
    }

    // A wavetable oscillator's band level (the most harmonics that stay
    // under the limit at this pitch) and its two frames either side of the
    // position.
    void table(Voice& s, int o, const OscSet& c, double inc) {
        const double x = kTop * inc / limit;
        int level = x <= 1.0 ? 0 : ceilToInt(log2(x));
        level = clamp(level, 0, kLevels - 1);
        const double at = clamp(c.position, 0.0, 1.0) * (kFrames - 1);
        const int f0 = min(floorToInt(at), kFrames - 2);
        s.blend[o] = at - f0;
        s.size[o] = levelSize[level];
        s.base0[o] = (c.table * kFrames + f0) * kPerFrame + levelAt[level];
        s.base1[o] = s.base0[o] + kPerFrame;
    }

    // The formant bank's three bands for a vowel 0..4 (a e i o u, averages
    // of adult speakers), moved by shift semitones.
    void formants(FilterNow& n, double vowel, double shift) {
        const double v = clamp(vowel, 0.0, 4.0);
        const int i = min(floorToInt(v), 3);
        const double t = v - i;
        const double up = exp2(shift / 12.0);
        for (int b = 0; b < 3; ++b) {
            const double hz = lerp(formantHz(i, b), formantHz(i + 1, b), t) * up;
            const double h = min(hz, 0.45 * rate);
            n.fg[b] = tan(pi * h / rate);
            // Speech's bandwidths, widened to a Q of at most 6 so the
            // harmonics of a low note still find the peaks.
            const double width = max(b == 0 ? 80.0 : b == 1 ? 90.0 : 120.0, h / 6.0);
            n.fk[b] = width / h;
        }
    }
    double formantHz(int vowel, int band) {
        if (vowel == 0) return band == 0 ? 730.0 : band == 1 ? 1090.0 : 2440.0;
        if (vowel == 1) return band == 0 ? 530.0 : band == 1 ? 1840.0 : 2480.0;
        if (vowel == 2) return band == 0 ? 270.0 : band == 1 ? 2290.0 : 3010.0;
        if (vowel == 3) return band == 0 ? 570.0 : band == 1 ? 840.0 : 2410.0;
        return band == 0 ? 300.0 : band == 1 ? 870.0 : 2240.0;
    }

    // --- Audio --------------------------------------------------------------------------------

    double filterOne(Filter& f, const FilterSet& c, const FilterNow& n, double x) {
        if (c.type == kLadder) return f.ladder(x, n.g, n.k, n.drive, c.steep);
        if (c.type == kSvf) return f.svf(x * n.drive, n.g, n.k, c.mode);
        if (c.type == kComb) return f.combFilter(x * n.drive, n.delay, n.feedback, n.damping);
        const double one = f.formantBand(0, x, n.fg[0], n.fk[0]);
        const double two = f.formantBand(1, x, n.fg[1], n.fk[1]);
        const double three = f.formantBand(2, x, n.fg[2], n.fk[2]);
        return (one + 0.6 * two + 0.35 * three) * 2.5 * n.drive;
    }

    // One channel through the filters as routed: a into filter 1, b into
    // filter 2, d past both.
    double filters(Voice& s, int c, double a, double b, double d) {
        const bool parallel = routing.index() == 1;
        double one = a;
        if (filt[0].type != kOff) one = filterOne(s.filter[c], filt[0], s.now[0], a);
        if (filt[1].type == kOff) return one + b + d;
        if (parallel) return one + filterOne(s.filter[2 + c], filt[1], s.now[1], b) + d;
        return filterOne(s.filter[2 + c], filt[1], s.now[1], one + b) + d;
    }

    double shape(double x, int type, double gain, double bias, double mixed) {
        const double y = driveCurve(type, x * gain, bias) / sqrt(max(1.0, gain));
        return x + (y - x) * mixed;
    }

    void play(int v, int frames) {
        Voice& s = voice[v];
        const int dtype = driveType.index();
        const double dgain = dbToGain(driveAmount);
        const double dbias = driveBias;
        const double dmix = driveMix;
        for (int i = 0; i < frames; ++i) {
            if (s.tickLeft == 0) {
                control(v, i);
                s.tickLeft = kTick;
                if (s.done) return;
            }
            --s.tickLeft;
            // Noise is made at the plugin's rate and held across the
            // oversampled frame, so it sounds the same at every quality.
            for (int o = 0; o < 4; ++o) {
                if (s.type[o] != kNoise) continue;
                const int col = osc[o].color;
                double y = 0.0;
                if (col == 0)
                    y = s.noises[o].white();
                else if (col == 1)
                    y = s.noises[o].pink();
                else if (col == 2)
                    y = s.noises[o].brown();
                else {
                    const double p = s.noises[o].pink();
                    y = (p - s.lastPink[o]) * 0.7;
                    s.lastPink[o] = p;
                }
                s.noise[o] = y * 2.0;
            }
            for (int k = 0; k < factor; ++k) {
                double aL = 0.0;
                double aR = 0.0;
                double bL = 0.0;
                double bR = 0.0;
                double dL = 0.0;
                double dR = 0.0;
                // From Osc 4 down, so a higher oscillator modulates a lower
                // one with this sample (a lower one a higher one with the
                // last), and Osc 1 wraps last, for the others' sync.
                for (int o = 3; o >= 0; --o) {
                    const int type = s.type[o];
                    if (type == kOff) continue;
                    const OscSet& c = osc[o];
                    const int n = s.copies[o];
                    const double depth = s.fm[o];
                    const int src = s.from[o] * 8;
                    const int srcCopies = max(1, s.copies[s.from[o]]);
                    double sumL = 0.0;
                    double sumR = 0.0;
                    for (int u = 0; u < n; ++u) {
                        const int x = o * 8 + u;
                        const double dt = s.inc[x];
                        s.inc[x] = dt + s.incStep[x];
                        double ph = s.phase[x];
                        if (c.sync && type == kAnalog && s.wrapped[u] >= 0.0)
                            ph = s.wrapped[u] * dt;
                        const double pm =
                            depth == 0.0 ? 0.0 : depth * s.out[src + min(u, srcCopies - 1)];
                        double y = 0.0;
                        if (type == kAnalog || type == kSub) {
                            y = analogWave(c.wave, pm == 0.0 ? ph : wrap01(ph + pm), dt, c.width);
                        } else if (type == kWavetable) {
                            const double tp = pm == 0.0 ? ph : wrap01(ph + pm / s.mult[o]);
                            y = wave(s, o, tp);
                        } else if (type == kFm) {
                            const double self = c.feedback * 0.5 * pi * (s.fb1[x] + s.fb2[x]);
                            y = sin(twoPi * (ph + pm) + self);
                            s.fb2[x] = s.fb1[x];
                            s.fb1[x] = y;
                        } else {
                            y = s.noise[o];
                        }
                        s.out[x] = y;
                        ph += dt;
                        if (ph >= 1.0) {
                            ph -= 1.0;
                            if (o == 0) s.wrapped[u] = ph / dt;
                        } else if (o == 0) {
                            s.wrapped[u] = -1.0;
                        }
                        s.phase[x] = ph;
                        sumL += y * s.panL[x];
                        sumR += y * s.panR[x];
                    }
                    const double g = s.gain[o];
                    s.gain[o] = g + s.gainStep[o];
                    sumL *= g;
                    sumR *= g;
                    if (c.direct) {
                        dL += sumL;
                        dR += sumR;
                    } else {
                        if (c.toA) {
                            aL += sumL;
                            aR += sumR;
                        }
                        if (c.toB) {
                            bL += sumL;
                            bR += sumR;
                        }
                    }
                }
                double yL = filters(s, 0, aL, bL, dL);
                double yR = s.narrow ? yL : filters(s, 1, aR, bR, dR);
                for (int f = 0; f < 2; ++f) {
                    FilterNow& n = s.now[f];
                    n.g += n.gStep;
                    n.k += n.kStep;
                    n.delay += n.delayStep;
                }
                if (dtype != 0) {
                    yL = shape(yL, dtype, dgain, dbias, dmix);
                    yR = s.narrow ? yL : shape(yR, dtype, dgain, dbias, dmix);
                }
                s.ampL += s.ampLStep;
                s.ampR += s.ampRStep;
                const double l = yL * s.ampL;
                const double r = yR * s.ampR;
                s.lastL = l;
                s.lastR = r;
                const int at = i * factor + k;
                mixL[at] += l;
                mixR[at] += r;
            }
        }
    }

    // A wavetable's sample at table phase tp: between two samples of each
    // frame, and between the frames.
    double wave(Voice& s, int o, double tp) {
        const int n = s.size[o];
        const double x = tp * n;
        const int i = min(floorToInt(x), n - 1);
        const double a = x - i;
        const int at0 = s.base0[o] + i;
        const int at1 = s.base1[o] + i;
        const double v0 = tables[at0] + a * (tables[at0 + 1] - tables[at0]);
        const double v1 = tables[at1] + a * (tables[at1 + 1] - tables[at1]);
        return v0 + s.blend[o] * (v1 - v0);
    }

    void process(Block& io) {
        const int n = io.frames * factor;
        for (int k = 0; k < n; ++k) {
            mixL[k] = 0.0;
            mixR[k] = 0.0;
        }
        for (int v = 0; v < voiceCount(); ++v) {
            Voice& s = voice[v];
            if (!s.sounding || !voicePlaying(v)) continue;
            play(v, io.frames);
            if (s.done) {
                silence(s);
                voiceFinished(v);
            }
        }
        for (int k = 0; k < n; ++k) {
            if (restL == 0.0 && restR == 0.0) break;
            mixL[k] += restL;
            mixR[k] += restR;
            restL *= restFade;
            restR *= restFade;
            if (abs(restL) + abs(restR) < 1e-9) {
                restL = 0.0;
                restR = 0.0;
            }
        }
        for (int i = 0; i < io.frames; ++i) {
            // Long silent: nothing to bring down, and the downsampler has
            // long since rung out.
            bool any = false;
            for (int k = 0; k < factor; ++k)
                if (mixL[i * factor + k] != 0.0 || mixR[i * factor + k] != 0.0) any = true;
            if (any)
                quiet = 0;
            else if (quiet < 4096)
                ++quiet;
            if (quiet == 4095) {
                over[0].clear();
                over[1].clear();
            }
            double l = 0.0;
            double r = 0.0;
            if (quiet < 4096) {
                over[0].up(0.0);
                over[1].up(0.0);
                for (int k = 0; k < factor; ++k) {
                    over[0].set(k, mixL[i * factor + k]);
                    over[1].set(k, mixR[i * factor + k]);
                }
                l = over[0].down();
                r = over[1].down();
            }
            const double g = dbToGain(output.at(i));
            if (io.channels == 1) {
                io[0][i] = safety((l + r) * 0.5 * g);
            } else {
                io[0][i] = safety(l * g);
                io[1][i] = safety(r * g);
            }
        }
    }

    // Untouched below -1 dBFS, then bent smoothly toward full scale.
    double safety(double x) {
        const double t = 0.891250938;
        const double a = abs(x);
        if (a <= t) return x;
        const double y = t + (1.0 - t) * fastTanh((a - t) / (1.0 - t));
        return x < 0.0 ? -y : y;
    }

    // --- The wavetables -----------------------------------------------------------------------

    void makeTables() {
        int at = 0;
        int size = kFft;
        for (int l = 0; l < kLevels; ++l) {
            levelSize[l] = max(256, size);
            levelAt[l] = at;
            at += levelSize[l] + 1;
            size = size / 2;
        }
        tables.resize(kTables * kFrames * kPerFrame);
        re.resize(kFft);
        im.resize(kFft);
        twiddleC.resize(kFft / 2);
        twiddleS.resize(kFft / 2);
        for (int j = 0; j < kFft / 2; ++j) {
            twiddleC[j] = cos(twoPi * j / kFft);
            twiddleS[j] = sin(twoPi * j / kFft);
        }
        for (int t = 0; t < kTables; ++t)
            for (int f = 0; f < kFrames; ++f) {
                recipe(t, f / (kFrames - 1.0));
                const int base = (t * kFrames + f) * kPerFrame;
                // One gain for every level of a frame, from the full band, so
                // the levels match: as loud as a saw (an RMS of 0.577), so a
                // sweep through the table holds its level, with the peak
                // kept within 2.
                double gain = 0.0;
                int top = kTop;
                for (int l = 0; l < kLevels; ++l) {
                    const int size = levelSize[l];
                    renderFrame(top, size);
                    if (l == 0) {
                        double peak = 0.0;
                        double power = 0.0;
                        for (int i = 0; i < size; ++i) {
                            peak = max(peak, abs(re[i]));
                            power += re[i] * re[i];
                        }
                        const double rms = sqrt(power / size);
                        gain = rms > 0.0 ? min(0.57735 / rms, 2.0 / peak) : 0.0;
                    }
                    for (int i = 0; i < size; ++i)
                        tables[base + levelAt[l] + i] = static_cast<float>(re[i] * gain);
                    tables[base + levelAt[l] + size] = tables[base + levelAt[l]];
                    top = top / 2;
                }
            }
    }

    // The frame with harmonics 1 .. top of amp and phs, size samples, into re.
    void renderFrame(int top, int size) {
        for (int i = 0; i < size; ++i) {
            re[i] = 0.0;
            im[i] = 0.0;
        }
        for (int k = 1; k <= top && k < size / 2; ++k) {
            // a sin(2 pi k t + phase) is half its amplitude at k and at
            // size - k, conjugate.
            const double a = amp[k] * 0.5;
            re[k] = a * sin(phs[k]);
            im[k] = -a * cos(phs[k]);
            re[size - k] = re[k];
            im[size - k] = -im[k];
        }
        transform(size);
    }

    // re + i im, n points (a power of two up to kFft), to its inverse
    // transform, unscaled: x[t] is the sum of X[k] e^(2 pi i k t / n).
    void transform(int n) {
        int j = 0;
        for (int i = 1; i < n; ++i) {
            int bit = n >> 1;
            while ((j & bit) != 0) {
                j = j ^ bit;
                bit = bit >> 1;
            }
            j = j ^ bit;
            if (i < j) {
                const double r = re[i];
                re[i] = re[j];
                re[j] = r;
                const double m = im[i];
                im[i] = im[j];
                im[j] = m;
            }
        }
        for (int len = 2; len <= n; len = len * 2) {
            const int half = len / 2;
            const int stride = idiv(kFft, len);
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k) {
                    const double wr = twiddleC[k * stride];
                    const double wi = twiddleS[k * stride];
                    const int a = i + k;
                    const int b = a + half;
                    const double xr = re[b] * wr - im[b] * wi;
                    const double xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
        }
    }

    // Table t at position p (0..1): its harmonics into amp and phs.
    void recipe(int t, double p) {
        amp.fill(0.0);
        phs.fill(0.0);
        if (t == 0) {
            // Harmonic ramp: a saw (1/k) rolling off to a sine.
            const double e = 1.0 + 8.0 * p * p;
            for (int k = 1; k <= kTop; ++k) {
                amp[k] = pow(k, -e);
                phs[k] = pi;
            }
        } else if (t == 1) {
            // PWM: a pulse narrowing from 50 % to 5 %.
            const double w = 0.5 - 0.45 * p;
            for (int k = 1; k <= kTop; ++k) {
                const double a = 4.0 / (pi * k) * sin(pi * k * w);
                amp[k] = abs(a);
                phs[k] = -pi * k * w + pi / 2.0 + (a < 0.0 ? pi : 0.0);
            }
        } else if (t == 2) {
            // Formant: a bright source through vowel formants a to u, for a
            // fundamental of about 110 Hz.
            const double x = p * 4.0;
            const int v = min(floorToInt(x), 3);
            const double fr = x - v;
            for (int k = 1; k <= kTop; ++k) {
                const double hz = k * 110.0;
                double env = 0.03;
                for (int b = 0; b < 3; ++b) {
                    const double f = lerp(formantHz(v, b), formantHz(v + 1, b), fr);
                    const double width = b == 0 ? 90.0 : b == 1 ? 110.0 : 160.0;
                    const double d = (hz - f) / width;
                    env += (b == 0 ? 1.0 : b == 1 ? 0.5 : 0.3) / (1.0 + d * d);
                }
                amp[k] = env / sqrt(k);
            }
        } else if (t == 3) {
            // Bell: hum, prime, tierce, quint, nominal and above, in eighths
            // of the note (the table plays an eighth of its pitch);
            // position brightens and stretches the upper partials.
            for (int i = 0; i < 12; ++i) {
                const double r = bellRatio(i) * (1.0 + 0.04 * p * (bellRatio(i) - 1.0));
                const int k = roundToInt(r * 8.0);
                const double dark = exp(-i * (1.0 - p) * 0.35);
                if (k >= 1 && k <= kTop) amp[k] += bellLevel(i) * dark;
            }
        } else if (t == 4) {
            // Organ: nine drawbars (16' to 1') on harmonics of half the note,
            // through eight registrations, 3 dB a step.
            const double x = p * 7.0;
            const int r = min(floorToInt(x), 6);
            const double fr = x - r;
            for (int d = 0; d < 9; ++d) {
                const double level = lerp(drawbar(r, d), drawbar(r + 1, d), fr);
                if (level <= 0.0) continue;
                amp[drawbarHarmonic(d)] += pow(10.0, -(8.0 - level) * 3.0 / 20.0);
            }
        } else if (t == 5) {
            // Digital: a ramp through a sample-and-hold and a quantizer that
            // coarsen along the table (5 to 3 bits, 16 to 6 steps a cycle).
            const double levels = exp2(4.0 - 2.0 * p);
            const int steps = roundToInt(exp2(4.0 - 1.415 * p));
            const int hold = idiv(kFft, max(1, steps));
            double mean = 0.0;
            for (int i = 0; i < kFft; ++i) {
                const double x = 2.0 * (idiv(i, hold) * hold) / kFft - 1.0;
                re[i] = round(x * levels) / levels;
                im[i] = 0.0;
                mean += re[i];
            }
            for (int i = 0; i < kFft; ++i) re[i] -= mean / kFft;
            // The forward transform is the inverse's conjugate.
            transform(kFft);
            for (int k = 1; k <= kTop; ++k) {
                const double cr = re[k] * 2.0 / kFft;
                const double ci = -im[k] * 2.0 / kFft;
                amp[k] = sqrt(cr * cr + ci * ci);
                phs[k] = atan2(ci, cr) + pi / 2.0;
            }
        } else {
            // Spectral comb: a 1/sqrt-ish series through a comb whose teeth
            // close in from every 16 harmonics to every 3, its phases
            // Schroeder's (pi k^2 / n), which keep the wave from piling into
            // one spike.
            const double period = 16.0 - 13.0 * p;
            for (int k = 1; k <= kTop; ++k) {
                const double c = 0.5 + 0.5 * cos(twoPi * (k - 1) / period);
                amp[k] = c * c / pow(k, 0.6);
                phs[k] = pi * k * k / kTop;
            }
        }
    }

    double bellRatio(int i) {
        if (i == 0) return 0.5;
        if (i == 1) return 1.0;
        if (i == 2) return 1.2;
        if (i == 3) return 1.5;
        if (i == 4) return 2.0;
        if (i == 5) return 2.5;
        if (i == 6) return 2.67;
        if (i == 7) return 3.0;
        if (i == 8) return 4.0;
        if (i == 9) return 5.33;
        if (i == 10) return 6.5;
        return 8.0;
    }
    double bellLevel(int i) {
        if (i == 0) return 0.6;
        if (i == 1) return 1.0;
        if (i == 2) return 0.8;
        if (i == 3) return 0.5;
        if (i == 4) return 0.9;
        if (i == 5) return 0.35;
        if (i == 6) return 0.3;
        if (i == 7) return 0.25;
        if (i == 8) return 0.2;
        if (i == 9) return 0.12;
        if (i == 10) return 0.08;
        return 0.05;
    }
    int drawbarHarmonic(int d) {
        if (d == 0) return 1;
        if (d == 1) return 3;
        if (d == 2) return 2;
        if (d == 3) return 4;
        if (d == 4) return 6;
        if (d == 5) return 8;
        if (d == 6) return 10;
        if (d == 7) return 12;
        return 16;
    }
    // Registration r's drawbar d, 0..8.
    double drawbar(int r, int d) {
        const Array<int, 72> regs = {0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 0, 0, 0, 0, 0,
                                     6, 0, 8, 6, 0, 4, 0, 0, 0, 8, 4, 8, 6, 0, 0, 0, 0, 0,
                                     8, 8, 8, 0, 0, 0, 0, 0, 0, 8, 8, 8, 8, 0, 0, 0, 0, 4,
                                     8, 6, 8, 8, 6, 8, 0, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
        return regs[r * 9 + d];
    }

    // --- Factory sounds ---------------------------------------------------------------------

    void factoryPatches(Patches& p) {
        p.add("Dune Pad")
            .set(unison, 3)
            .set(detune, 12)
            .set(spread, 70)
            .set(drift, 25)
            .set(o1Fine, -7)
            .set(o2Type, "analog")
            .set(o2Fine, 7)
            .set(o2Level, -6)
            .set(f1Cutoff, 1200)
            .set(f1Resonance, 25)
            .set(f1Env, 2)
            .set(envAttack, 1500)
            .set(envDecay, 3000)
            .set(envSustain, 30)
            .set(envRelease, 2000)
            .set(ampAttack, 600)
            .set(ampDecay, 1200)
            .set(ampSustain, 80)
            .set(ampRelease, 1500);
        p.add("Pluck Bass")
            .set(o1Retrigger, true)
            .set(o2Type, "sub")
            .set(o2Wave, "sine")
            .set(o2Level, -2)
            .set(o2Retrigger, true)
            .set(f1Cutoff, 220)
            .set(f1Resonance, 30)
            .set(f1Env, 3.5)
            .set(f1Keytrack, 50)
            .set(envDecay, 250)
            .set(envSustain, 0)
            .set(envRelease, 200)
            .set(ampDecay, 450)
            .set(ampSustain, 50)
            .set(ampRelease, 120)
            .set(ampVelocity, 60)
            .set(o1Level, 1);
        p.add("Acid Line")
            .set(mode, "legato")
            .set(glide, 70)
            .set(o1Wave, "saw")
            .set(o1Retrigger, true)
            .set(f1Cutoff, 320)
            .set(f1Resonance, 80)
            .set(f1Env, 3)
            .set(f1Velocity, 1)
            .set(envDecay, 220)
            .set(envSustain, 0)
            .set(driveType, "soft")
            .set(driveAmount, 9)
            .set(ampDecay, 300)
            .set(ampSustain, 70)
            .set(ampRelease, 60)
            .set(o1Level, 0);
        p.add("Supersaw")
            .set(unison, 7)
            .set(detune, 28)
            .set(spread, 90)
            .set(o1Level, -4.5)
            .set(f1Slope, "db12")
            .set(f1Cutoff, 6000)
            .set(f1Resonance, 5)
            .set(f1Env, 1)
            .set(envDecay, 800)
            .set(envSustain, 40)
            .set(ampAttack, 5)
            .set(ampDecay, 500)
            .set(ampSustain, 85)
            .set(ampRelease, 400);
        p.add("Sub Bass")
            .set(o1Wave, "sine")
            .set(o1Level, -10)
            .set(o1Retrigger, true)
            .set(f1Type, "off")
            .set(ampAttack, 3)
            .set(ampDecay, 300)
            .set(ampSustain, 100)
            .set(ampRelease, 120)
            .set(ampVelocity, 30);
        p.add("Tine Piano")
            .set(o1Type, "fm")
            .set(o1Level, -1)
            .set(o1Fm, 2.5)
            .set(o1FmFrom, "osc2")
            .set(o1Retrigger, true)
            .set(o2Type, "fm")
            .set(o2Ratio, 1)
            .set(o2Level, -60)
            .set(o2Retrigger, true)
            .set(o3Type, "fm")
            .set(o3Ratio, 14)
            .set(o3Level, -34)
            .set(o3Retrigger, true)
            .set(f1Type, "off")
            .set(envFm, 100)
            .set(envAttack, 0)
            .set(envDecay, 1600)
            .set(envSustain, 15)
            .set(envRelease, 400)
            .set(ampAttack, 1)
            .set(ampDecay, 4500)
            .set(ampSustain, 0)
            .set(ampRelease, 450)
            .set(ampVelocity, 70);
        p.add("Glass Bell")
            .set(o1Type, "fm")
            .set(o1Level, -7)
            .set(o1Fm, 3.5)
            .set(o1FmFrom, "osc2")
            .set(o1Retrigger, true)
            .set(o2Type, "fm")
            .set(o2Ratio, 3.5)
            .set(o2Level, -60)
            .set(o2Retrigger, true)
            .set(f1Type, "off")
            .set(envFm, 70)
            .set(envAttack, 0)
            .set(envDecay, 2500)
            .set(envSustain, 0)
            .set(envRelease, 2500)
            .set(ampAttack, 1)
            .set(ampDecay, 6000)
            .set(ampSustain, 0)
            .set(ampRelease, 2500);
        p.add("Vowel Lead")
            .set(mode, "legato")
            .set(glide, 40)
            .set(unison, 2)
            .set(detune, 8)
            .set(spread, 30)
            .set(f1Type, "formant")
            .set(f1Vowel, 1.5)
            .set(f1Keytrack, 30)
            .set(ampAttack, 30)
            .set(ampDecay, 400)
            .set(ampSustain, 85)
            .set(ampRelease, 250)
            .set(o1Level, -2);
        p.add("Strings")
            .set(unison, 5)
            .set(detune, 18)
            .set(spread, 80)
            .set(drift, 35)
            .set(o2Type, "analog")
            .set(o2Wave, "pulse")
            .set(o2Width, 30)
            .set(o2Level, -13)
            .set(f1Type, "svf")
            .set(f1Cutoff, 3500)
            .set(f1Resonance, 10)
            .set(f1Env, 0)
            .set(f1Keytrack, 40)
            .set(ampAttack, 350)
            .set(ampDecay, 1)
            .set(ampSustain, 100)
            .set(ampRelease, 900)
            .set(o1Level, -9);
        p.add("Noise Snare")
            .set(o1Type, "noise")
            .set(o1Level, 0)
            .set(o2Type, "analog")
            .set(o2Wave, "triangle")
            .set(o2Coarse, -5)
            .set(o2Level, -3)
            .set(o2Retrigger, true)
            .set(o2To, "direct")
            .set(f1Type, "svf")
            .set(f1Mode, "lowpass")
            .set(f1Cutoff, 6000)
            .set(f1Env, 0)
            .set(envPitch, 12)
            .set(envAttack, 0)
            .set(envDecay, 45)
            .set(envSustain, 0)
            .set(ampAttack, 0)
            .set(ampDecay, 190)
            .set(ampSustain, 0)
            .set(ampRelease, 120)
            .set(ampVelocity, 60)
            .set(f2Type, "svf")
            .set(f2Mode, "highpass")
            .set(f2Cutoff, 350)
            .set(f2Env, 0);
        p.add("Drawbar Organ")
            .set(o1Type, "wavetable")
            .set(o1Table, "organ")
            .set(o1Position, 70)
            .set(o1Level, -12)
            .set(f1Type, "off")
            .set(drift, 15)
            .set(driveType, "tape")
            .set(driveAmount, 4)
            .set(ampAttack, 4)
            .set(ampDecay, 1)
            .set(ampSustain, 100)
            .set(ampRelease, 60)
            .set(ampVelocity, 0);
    }
};
