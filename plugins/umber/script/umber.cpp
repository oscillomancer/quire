// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Umber: a tape machine, from the faint rounding of peaks that a well-kept
// modern machine gives a mix to a worn, underbiased, warbling cassette.
//
// Umber is an earth pigment, iron oxide with a little manganese: the same rust
// that coats a tape. No particular machine is copied. Each part of the path
// is modelled where it changes the sound, and fitted to published tape data
// (third-harmonic distortion against level, saturation at high frequencies,
// noise and print figures of a current studio tape at 30, 15 and 7.5 ips):
//
//   Record   the drive sets how hard the signal meets the tape: at 0 dB a
//            sine at -18 dBFS RMS records at the tape's reference fluxivity.
//            Record equalisation lifts the highs to make up for the tape's
//            losses at short wavelengths (fitted to the tape's saturation at
//            10 and 16 kHz) less the standard's time constant (NAB or IEC):
//            so highs saturate first, more so at slower speeds.
//   Tape     the oxide's magnetization: the particles are single-domain
//            needles with two states, so the anhysteretic curve is a tanh of
//            the field, sharpened by the tape's squareness. Bias decides how
//            much hysteresis is left: at the recommended bias the
//            irreversible magnetization follows that curve almost at once
//            (clean at low levels, about 2 dB more third harmonic per dB of
//            level, rounding only near the tape's limit); underbias pins it,
//            and the loop opens into the grain of unbiased tape.
//   Playback the inverse of the record equalisation, the playback head's
//            contour effect (a bump in the lows whose frequency follows the
//            speed), its gap and the aligned top end, bias's tilt of the
//            highs, and wear: spacing loss, azimuth error (highs lost within
//            each track, and between the two), magnetized heads (even
//            harmonics) and dropouts.
//   Motion   wow from the reels' turning and a slow drift, flutter from the
//            capstan, an idler and the tape's own scrape, all as a moving
//            delay read through a cubic at the oversampled rate.
//   Noise    hiss as the tape's particles give it through the playback
//            equalisation, and modulation noise riding on the signal.
//
// The record path, the tape and the moving read run oversampled: 4x with
// linear-phase filters and room for deep wow (about 8.5 ms of latency), or
// 2x with minimum-phase filters and the wow held to a few samples, under 10
// samples in all, for tracking.

constexpr double kRef = 0.17782794100389229;  // a sine at -18 dBFS RMS: its peak
constexpr double kRefRms = 0.12574334296829354;
constexpr int kLoop = 1024;  // the editor's trace of the tape's magnetization
constexpr int kTick = 32;    // frames between the transport's random parts

// Gains often hold still for many frames. Cache only exact repeats, so a
// glide still gets every distinct gain it asks for.
struct GainCache {
    double at = 1e100, value = 1.0;
    double get(double db) {
        if (db != at) {
            at = db;
            value = dbToGain(db);
        }
        return value;
    }
};

// A first-order section, g (1 + s/wa) / (1 + s/wb), by the bilinear transform
// with both corners prewarped: biproper, so its inverse is one too.
struct Section {
    double b0 = 1.0, b1 = 0.0, a1 = 0.0, z = 0.0;

    void design(double fa, double fb, double g, double fs) {
        const double top = 0.49 * fs;
        const double a = 1.0 / tan(pi * min(fa, top) / fs);
        const double b = 1.0 / tan(pi * min(fb, top) / fs);
        b0 = g * (1.0 + a) / (1.0 + b);
        b1 = g * (1.0 - a) / (1.0 + b);
        a1 = (1.0 - b) / (1.0 + b);
    }
    double process(double x) {
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
    // Its gain at frequency f, for the editor and the noise's calibration.
    double gainAt(double f, double fs) const {
        const double w = twoPi * f / fs;
        const double nr = b0 + b1 * cos(w), ni = -b1 * sin(w);
        const double dr = 1.0 + a1 * cos(w), di = -a1 * sin(w);
        return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    void clear() { z = 0.0; }
};

// --- the tape -----------------------------------------------------------------------------------

// The tapes: squareness (sharpens the knee), the field at reference level,
// the pinning left at the recommended bias, A-weighted hiss at 15 ips and
// modulation noise against the reference level, and how much shorter
// wavelengths cost them.
double tapeSquare(int t) { return t == 0 ? 0.24 : (t == 1 ? 0.16 : 0.08); }
double tapeField(int t) { return t == 0 ? 0.1978 : (t == 1 ? 0.2969 : 0.4440); }
double tapePin(int t) { return t == 0 ? 1e-5 : (t == 1 ? 3e-5 : 1e-4); }
double tapeHissDb(int t) { return t == 0 ? -60.5 : (t == 1 ? -57.0 : -52.0); }
double tapeModDb(int t) { return t == 0 ? -64.0 : (t == 1 ? -60.0 : -54.0); }
double tapeLoss(int t) { return t == 0 ? 1.0 : (t == 1 ? 1.1 : 1.3); }
// Third harmonic at 3% (MOL): its field against the reference's, in dB.
double tapeMolDb(int t) { return t == 0 ? 12.5 : (t == 1 ? 9.0 : 5.0); }

double speedIps(int s) { return s == 0 ? 30.0 : (s == 1 ? 15.0 : (s == 2 ? 7.5 : 3.75)); }
// How much harder each speed records a mid frequency for the same level:
// the tape's limit at 1 kHz falls as the wavelength shortens (the datasheet's
// MOL at 30, 15 and 7.5 ips; 3.75 extrapolated).
double speedFieldDb(int s) { return s == 0 ? 0.1 : (s == 1 ? 0.6 : (s == 2 ? 2.9 : 5.5)); }
// The time constant of each speed's standard, in seconds: 30 ips is AES's
// 17.5 us either way, and NAB adds a 3180 us lift of the lows below 15 ips.
double speedTau(int s, bool nab) {
    if (s == 0) return 17.5e-6;
    if (s == 1) return nab ? 50e-6 : 35e-6;
    if (s == 2) return nab ? 50e-6 : 70e-6;
    return 90e-6;
}
// Hiss against the reference level, A-weighted, for a modern tape at each
// speed with the IEC curve.
double speedHissDb(int s) { return s == 0 ? -63.0 : (s == 1 ? -60.5 : (s == 2 ? -58.0 : -55.0)); }
// How far the highs move as bias moves from peak to the recommended setting
// (the datasheet's delta S10), and the head bump's height.
double speedBiasTilt(int s) { return s == 0 ? 1.5 : (s == 1 ? 3.0 : (s == 2 ? 6.5 : 9.0)); }
double speedBumpDb(int s) { return s == 0 ? 1.2 : (s == 1 ? 1.6 : (s == 2 ? 2.0 : 2.2)); }
// The top end of an aligned machine at low level.
double speedTopHz(int s) {
    return s == 0 ? 30000.0 : (s == 1 ? 24000.0 : (s == 2 ? 17000.0 : 10500.0));
}

// The anhysteretic curve: a tanh of the field, squared off by squareness.
// Below 0.3 its series to the ninth power is as exact (within 2e-8) and
// cheaper.
double anhysteretic(double u, double sq) {
    const double y = u + sq * u * u * u;
    if (abs(y) >= 0.3) return tanh(y);
    const double y2 = y * y;
    return y * (1.0 +
                y2 * (-1.0 / 3.0 + y2 * (2.0 / 15.0 + y2 * (-17.0 / 315.0 + y2 * 62.0 / 2835.0))));
}

// A sine turned by rotation each frame (a pair of numbers, kept on the unit
// circle every kTick frames), for the transport's steady parts.
struct Phasor {
    double c = 1.0, s = 0.0, dc = 1.0, ds = 0.0;
    void start(double cycles) {
        c = cos(twoPi * cycles);
        s = sin(twoPi * cycles);
    }
    void rate(double cyclesPerFrame) {
        dc = cos(twoPi * cyclesPerFrame);
        ds = sin(twoPi * cyclesPerFrame);
    }
    void next() {
        const double nc = c * dc - s * ds;
        s = s * dc + c * ds;
        c = nc;
    }
    void hold() {
        const double g = 1.0 / sqrt(c * c + s * s);
        c *= g;
        s *= g;
    }
};

// The oxide of one track: irreversible magnetization pinned until the field
// pushes it on, toward the anhysteretic value (an exact exponential step in
// the field, so a narrow loop never goes unstable), and a reversible part
// that follows the field at once.
struct Oxide {
    double h = 0.0, mirr = 0.0, m = 0.0;
    void clear() {
        h = 0.0;
        mirr = 0.0;
        m = 0.0;
    }
    double step(double hn, double sq, double pin) {
        const double dh = hn - h;
        h = hn;
        const double man = anhysteretic(hn, sq);
        const double diff = man - mirr;
        if (dh * diff > 0.0) {
            const double t = abs(dh) / pin;
            mirr = t > 20.0 ? man : man - diff * exp(-t);
        }
        m = mirr + 0.3 * (man - mirr);
        return m;
    }
};

// The playback head's filters as the settings put them, for the audio and
// for the editor's copy.
struct Head {
    Biquad low, bump, dip, top, wear, tilt;
    bool topOn = false, wearOn = false;
    void design(int s, int t, double amount, double wearHz, double tiltDb, double rate) {
        const double v = speedIps(s) / 15.0;
        const double fb = 3.8 * speedIps(s);
        const double bumpDb = speedBumpDb(s) * amount;
        const double topHz = speedTopHz(s) / tapeLoss(t);
        // A corner past 0.45 of the rate would only droop the top octave:
        // there the machine is flat, so the filters stay out.
        topOn = topHz < 0.45 * rate;
        wearOn = wearHz < 0.45 * rate;
        low.highpass(max(5.0, 0.36 * fb * min(1.0, amount)), 0.75);
        bump.peak(fb, 1.2, bumpDb);
        dip.peak(2.1 * fb, 2.0, -0.4 * bumpDb);
        if (topOn) top.lowpass(topHz, 0.75);
        if (wearOn) wear.lowpass(wearHz, 0.6);
        tilt.highShelf(min(5000.0 * v, 0.4 * rate), 0.6, tiltDb);
    }
    double process(double x) {
        double y = x;
        if (topOn) y = top.process(y);
        if (wearOn) y = wear.process(y);
        y = tilt.process(y);
        return dip.process(bump.process(low.process(y)));
    }
    double responseDb(double hz) const {
        double db =
            low.responseDb(hz) + bump.responseDb(hz) + dip.responseDb(hz) + tilt.responseDb(hz);
        if (topOn) db += top.responseDb(hz);
        if (wearOn) db += wear.responseDb(hz);
        return db;
    }
    void clear() {
        low.clear();
        bump.clear();
        dip.clear();
        top.clear();
        wear.clear();
        tilt.clear();
    }
};

// Bias's tilt of the highs, in dB: up toward the peak of sensitivity below
// the recommended bias, then down again; down above it.
double biasTiltDb(int s, double b) {
    const double dS = speedBiasTilt(s);
    if (b < 1.0) return b >= 0.7 ? dS * (1.0 - b) / 0.3 : dS * max(0.0, (b - 0.4) / 0.3);
    return -dS * (b - 1.0) / 0.3;
}

// Every frequency's level against bias: far under the recommended bias it
// falls; over it, a little.
double biasLevelDb(double b) {
    if (b < 0.7) return -6.0 * (0.7 - b) * (0.7 - b) / 0.04;
    if (b > 1.0) return -0.8 * (b - 1.0) / 0.5;
    return 0.0;
}

// The frequency at which spacing loss (54.6 d / wavelength dB) and the
// azimuth's own gap loss together take 3 dB: the wear's low-pass.
double lossCorner(double spacing, double azGap, double speedMs) {
    if (spacing <= 0.0 && azGap <= 0.0) return 1e9;
    double lo = 100.0, hiHz = 1e6;
    for (int i = 0; i < 40; ++i) {
        const double f = sqrt(lo * hiHz);
        const double x = pi * azGap * f / speedMs;
        const double sinc = x < 1e-6 ? 1.0 : abs(sin(x) / x);
        const double lossDb = 54.6 * spacing * f / speedMs - gainToDb(max(sinc, 1e-6));
        if (lossDb > 3.0)
            hiHz = f;
        else
            lo = f;
    }
    return lo;
}

// Wear's low-pass corner for a setting.
double wearCorner(double w, int s) {
    const double theta = w * 10.0 / 60.0 * pi / 180.0;  // up to 10 arcminutes
    return lossCorner(w * 1.2e-6, 0.002 * tan(theta), speedIps(s) * 0.0254);
}

// The record equalisation's corners for a tape and speed: two zeros that make
// up the tape's losses at short wavelengths, a pole pair above the band, the
// standard's time constant and NAB's lift of the lows.
struct Emphasis {
    Section e1, e2, e3, e4;
    // The record curve, or (inverse) the playback's, which undoes it exactly.
    void design(int t, int s, bool nab, double hi, bool inverse) {
        const double v = speedIps(s) / 15.0;
        const double loss = tapeLoss(t);
        const double z1 = 2374.0 * v / loss, z2 = 9027.0 * v / loss;
        const double p = min(80000.0 * v, 0.42 * hi);
        const double fTau = 1.0 / (twoPi * speedTau(s, nab));
        const double edge = 0.45 * hi;
        const double eps = 0.316;
        const double fLow = 1.0 / (twoPi * 3180e-6);
        if (inverse) {
            e1.design(p, z1, 1.0, hi);
            e2.design(p, z2, 1.0, hi);
            e3.design(fTau, edge, 1.0, hi);
            if (nab)
                e4.design(fLow * eps, fLow, eps, hi);
            else
                e4.design(fLow, fLow, 1.0, hi);
        } else {
            e1.design(z1, p, 1.0, hi);
            e2.design(z2, p, 1.0, hi);
            e3.design(edge, fTau, 1.0, hi);
            if (nab)
                e4.design(fLow, fLow * eps, 1.0 / eps, hi);
            else
                e4.design(fLow, fLow, 1.0, hi);
        }
    }
    double process(double x) { return e4.process(e3.process(e2.process(e1.process(x)))); }
    void clear() {
        e1.clear();
        e2.clear();
        e3.clear();
        e4.clear();
    }
    double gainAt(double f, double hi) const {
        return e1.gainAt(f, hi) * e2.gainAt(f, hi) * e3.gainAt(f, hi) * e4.gainAt(f, hi);
    }
};

// A-weighting's gain at f (IEC 61672).
double aWeight(double f) {
    const double f2 = f * f;
    const double num = 12194.0 * 12194.0 * f2 * f2;
    const double den = (f2 + 20.6 * 20.6) * sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) *
                       (f2 + 12194.0 * 12194.0);
    return num / den * 1.2589254117941673;  // +2.0 dB: 0 dB at 1 kHz
}

// What one track's record path, tape and playback equalisation hold.
struct Track {
    Oversampler os;
    Emphasis rec, play;  // record equalisation, and playback's inverse of it
    Oxide oxide;
    Buffer<double> ring;  // the moving read, at the oversampled rate
    int at = 0;
    Section hissTau, hissLow, hissTop;  // the hiss through the playback equalisation
    Head head;
    double modZ1 = 0.0, modZ2 = 0.0;  // modulation noise, low-passed
    double vu1 = 0.0, vu2 = 0.0;      // the VU meter's two poles
    double needle = 0.0;
    void clear() {
        os.clear();
        rec.clear();
        play.clear();
        oxide.clear();
        for (int i = 0; i < ring.size(); ++i) ring[i] = 0.0;
        at = 0;
        hissTau.clear();
        hissLow.clear();
        hissTop.clear();
        head.clear();
        modZ1 = 0.0;
        modZ2 = 0.0;
        vu1 = 0.0;
        vu2 = 0.0;
        needle = 0.0;
    }
};

// The palette: the house's earth, with umber and its oxides.
constexpr int kInk = 0x1A1411;
constexpr int kGroundTop = 0x2B221B;
constexpr int kGroundBottom = 0x201914;
constexpr int kGraphTop = 0x1D1713;
constexpr int kGraphBottom = 0x130F0C;
constexpr int kGrid = 0x2B231D;
constexpr int kGridLit = 0x3E332A;
constexpr int kKnobLight = 0x54463B;
constexpr int kKnobDark = 0x2A221C;
constexpr int kMeterLow = 0x6E7A4B;
constexpr int kMeterHigh = 0xD8CE9C;
constexpr int kUmber = 0xC08A55;  // burnt umber, lit
constexpr int kOxide = 0x7A4A2E;  // the tape's coat
constexpr int kRust = 0xC4673F;
constexpr int kCream = 0xEADFC8;  // the meters' faces
constexpr int kMoss = 0xA9B86C;

struct Umber : Effect {
    static constexpr Info info = {
        .name = "Umber",
        .id = "com.suppleaudio.umber",
        .vendor = "Supple Audio",
        .version = "1.0.0",
        .description =
            "A tape machine modelled from the oxide up: record equalisation, a magnetic core "
            "whose hysteresis follows the bias, the playback head's bump and losses, wow and "
            "flutter, hiss and wear, at four speeds and three tapes, from a faint rounding of "
            "peaks to a worn, warbling cassette.",
        .features = {"distortion", "tape"}};

    // --- record -----------------------------------------------------------------------------

    Param drive = decibels("Drive", -24, 24, 0)
                      .smoothed(30)
                      .in("Record")
                      .role({"level"})
                      .help(
                          "How hard the signal meets the tape: at 0 dB a sine at -18 dBFS RMS "
                          "records at the tape's reference level.");
    Param tape = choice("Tape", {"Modern", "Classic", "Vintage"}, 0)
                     .in("Record")
                     .help(
                         "The formulation: modern has 12.5 dB of headroom over reference, "
                         "classic 9 and vintage 5, each softer and noisier than the last.");
    Param bias = percent("Bias", 100, 50, 150)
                     .in("Record")
                     .help(
                         "The bias against the recommended setting: under it, brighter and "
                         "grittier, distorting at low levels too; over it, darker and cleaner.");
    Param autoGain = toggle("Auto Gain", true)
                         .in("Record")
                         .help(
                             "Turns the playback down as the drive turns the recording up, so "
                             "the level holds while the tape works harder.");

    // --- the machine -------------------------------------------------------------------------

    Param speed = choice("Speed", {"30 ips", "15 ips", "7.5 ips", "3.75 ips"}, 1)
                      .keys({"ips30", "ips15", "ips7_5", "ips3_75"})
                      .in("Machine")
                      .help(
                          "Tape speed: slower lowers the head bump, saturates the highs "
                          "sooner, loses more top, hisses more and wavers more.");
    Param emphasis = choice("Emphasis", {"NAB", "IEC"}, 1)
                         .keys({"nab", "iec"})
                         .in("Machine")
                         .activeUnless(speed, {"ips30"})
                         .help(
                             "The equalisation standard: NAB lifts the lows into the tape (they "
                             "saturate sooner) and trades a little hiss for headroom in the "
                             "highs; 30 ips is AES.");
    Param headBump = percent("Head Bump", 100, 0, 200)
                         .in("Machine")
                         .help(
                             "The playback head's swell in the lows (and the dip above it), "
                             "where the speed puts it: 100% as a real head gives it.");
    Param wow = percent("Wow", 0.02, 0, 2)
                    .curved(Curve::Pow, 3)
                    .in("Machine")
                    .help(
                        "Slow pitch drift from the reels, as a peak speed error: 0.02% is a "
                        "good studio machine, 0.2% a cassette deck.");
    Param flutter = percent("Flutter", 0.02, 0, 1)
                        .curved(Curve::Pow, 3)
                        .in("Machine")
                        .help(
                            "Faster pitch waver from the capstan, idlers and the tape's "
                            "scrape, as a peak speed error.");
    Param wear = percent("Wear", 0).in("Machine").help(
        "Worn, dirty, misaligned and magnetized heads: duller highs, a lean "
        "between the channels, even harmonics, unsteadier speed, dropouts.");
    Param hiss = decibels("Hiss", -30, 12, -30)
                     .offAtMin()
                     .in("Machine")
                     .help(
                         "The tape's own noise, hiss and modulation noise, against what this "
                         "tape at this speed gives: 0 dB is real; off at the minimum.");

    // --- output -----------------------------------------------------------------------------

    Param output = decibels("Output", -24, 24, 0).smoothed(20).in("Output").help("The level out.");
    Param latency = choice("Latency", {"Full", "Low"}, 0)
                        .keys({"full", "low"})
                        .restarts()
                        .in("Output")
                        .help(
                            "Full: 4x linear-phase oversampling and room for deep wow (about "
                            "8.5 ms). Low: under 10 samples for tracking, 2x, wow held small.");

    // --- readouts ---------------------------------------------------------------------------

    Param vu = decibels("VU", -40, 12, -40)
                   .meter()
                   .help(
                       "The record level against the tape's reference, as a VU meter reads "
                       "it (0 VU is the reference level).");
    Param headroom = decibels("Headroom", -36, 48, 48)
                         .meter()
                         .help(
                             "How far the loudest recent peak, as the tape sees it (highs "
                             "lifted), stayed below the tape's 3% limit; negative is over.");
    Param saturation =
        decibels("Saturation", 0, 24, 0)
            .meter()
            .help("How much the tape rounded the loudest recent peaks, in dB of level lost.");

    // --- the audio code's -----------------------------------------------------------------------

    double rate = 48000.0, hi = 192000.0;
    int factor = 4;
    int center = 0;      // the moving read's resting delay, in base samples
    double reach = 0.0;  // how far it may move either way, in seconds
    bool unplayed = true;

    Array<Track, 2> track;
    Random random;  // the transport's
    Random noise;   // the tape's noise
    // The setup as last designed, the values gliding, and a switch of tape,
    // speed or standard: 1 while the output fades out, 2 while the input
    // fades back in.
    int tapeNow = 0, speedNow = 1, nabNow = 0;
    double biasNow = 1.0, bumpNow = 1.0, wearNow = 0.0, glideCoef = 0.1;
    int switching = 0;
    double fade = 1.0, fadeStep = 0.005;
    double field = 0.2, square = 0.24, pin = 1e-5, molField = 1.0;
    double hissGain = 0.0, modGain = 0.0, modScale = 1.0, hissOn = 0.0;
    double sensitivity = 1.0, dc = 0.0, dcOut = 0.0, azimuth = 0.0;
    double ips = 15.0;
    // The transport: the speed error's parts.
    Phasor reel, takeup, capstan, idler;
    double drift1 = 0.0, drift2 = 0.0, driftCoef = 0.0, driftScale = 1.0, driftTarget = 0.0;
    double scrape1 = 0.0, scrapeLow = 0.0, scrapeCoef = 0.0, scrapeLowCoef = 0.0;
    double scrapeScale = 1.0, scrapeTarget = 0.0;
    double travel = 0.0, travelKeep = 0.9999;  // the tape's displacement, in seconds
    double readNow = 0.0, readWas = 0.0;       // the read's delay, oversampled samples
    int tick = 0;
    // Dropouts.
    double dropDepth = 0.0, dropAt = 1.0, dropStep = 0.0, dropOther = 1.0;
    double dropChance = 0.0;
    double modCoef = 0.1;
    // Meters.
    double peakField = 0.0, peakKeep = 0.9999, rounding = 0.0, roundKeep = 0.9999;
    double roundRatio = 1.0;  // the rounding as a ratio, refreshed every kTick frames
    GainCache driveCache, outputCache;
    double vuCoef = 0.001;
    double tapeRun = 0.0;  // metres of tape played, for the editor's reels
    bool rolling = false;
    double quiet = 0.0;  // seconds since the input was last above silence

    // --- shared with the editor -------------------------------------------------------------

    Shared<double> vuLeft, vuRight, inLeft, inRight, outLeft, outRight;
    Shared<double> tapeMetres, sampleRateNow;
    Shared<bool> running;
    Shared<int> loopHead;
    SharedArray<float, 1024> loopH, loopM;
    Shared<double> loopField;  // the field at the tape's limit, to scale the trace
    int loopAt = 0;
    double peakInL = 0.0, peakInR = 0.0, peakOutL = 0.0, peakOutR = 0.0, fall = 0.999;

    void factoryPatches(Patches& p) {
        p.add("Mastering")
            .set(tape, "modern")
            .set(speed, "ips30")
            .set(drive, 0)
            .set(headBump, 60)
            .set(wow, 0.01)
            .set(flutter, 0.01);
        p.add("Mix Bus").set(tape, "modern").set(speed, "ips15").set(drive, 2);
        p.add("Peak Rounding")
            .set(tape, "modern")
            .set(speed, "ips15")
            .set(drive, 5)
            .set(wow, 0)
            .set(flutter, 0);
        p.add("Tracking")
            .set(tape, "modern")
            .set(speed, "ips15")
            .set(emphasis, "nab")
            .set(drive, 3)
            .set(latency, "low");
        p.add("Hot Drums")
            .set(tape, "classic")
            .set(speed, "ips15")
            .set(emphasis, "nab")
            .set(drive, 9)
            .set(headBump, 120)
            .set(output, 1.5);
        p.add("Fat Bass")
            .set(tape, "classic")
            .set(speed, "ips7_5")
            .set(emphasis, "nab")
            .set(drive, 6)
            .set(headBump, 140)
            .set(output, 2.5);
        p.add("Saturated")
            .set(tape, "modern")
            .set(speed, "ips7_5")
            .set(drive, 14)
            .set(wow, 0.04)
            .set(flutter, 0.04)
            .set(output, 3.5);
        p.add("Underbiased")
            .set(tape, "classic")
            .set(speed, "ips15")
            .set(bias, 65)
            .set(drive, 4)
            .set(output, 0.5);
        p.add("Cassette")
            .set(tape, "vintage")
            .set(speed, "ips3_75")
            .set(drive, -3)
            .set(wow, 0.12)
            .set(flutter, 0.1)
            .set(wear, 20)
            .set(hiss, 0)
            .set(output, 3);
        p.add("Worn Machine")
            .set(tape, "vintage")
            .set(speed, "ips7_5")
            .set(bias, 85)
            .set(drive, 4)
            .set(wow, 0.25)
            .set(flutter, 0.15)
            .set(wear, 70)
            .set(hiss, 4)
            .set(output, 3);
        p.add("Warped")
            .set(tape, "vintage")
            .set(speed, "ips3_75")
            .set(drive, -2)
            .set(wow, 1.4)
            .set(flutter, 0.35)
            .set(wear, 50)
            .set(hiss, 6)
            .set(output, 3);
        p.add("Clean Transfer")
            .set(tape, "modern")
            .set(speed, "ips30")
            .set(drive, -12)
            .set(headBump, 0)
            .set(wow, 0)
            .set(flutter, 0);
    }

    void prepare() {
        rate = sampleRate();
        // The tape runs near 176 to 192 kHz at full quality (4x at 44.1 and
        // 48 kHz, 2x at 88.2 and 96, none above), at half that for low
        // latency.
        const bool low = latency.index() == 1;
        int want = low ? 2 : 4;
        if (rate >= 120000.0)
            want = 1;
        else if (rate >= 60000.0)
            want = low ? 1 : 2;
        for (Track& t : track) {
            if (low)
                t.os.prepare(want);
            else
                t.os.prepare(want, Oversampler::Phase::Linear);
        }
        factor = track[0].os.factor();
        hi = rate * factor;
        const int osLatency = track[0].os.latency();
        center = low ? max(2, 9 - osLatency) : roundToInt(0.008 * rate);
        reach = (center - 1.5) / rate;
        setLatency(osLatency + center);
        const int size = (center * 2 + 8) * factor + 8;
        for (Track& t : track) t.ring.resize(size);
        random.seed(0x5eed);
        noise.seed(0x7a9e);
        peakKeep = exp(-1.0 / (0.3 * rate));
        roundKeep = peakKeep;
        vuCoef = 1.0 - exp(-1.0 / (0.065 * rate));
        fall = exp(-1.0 / (0.25 * rate));
        travelKeep = exp(-1.0 / (4.0 * rate));
        glideCoef = 1.0 - exp(-kTick / (0.03 * rate));
        fadeStep = 1.0 / (0.004 * rate);
        for (Track& t : track) t.clear();
        clearState();
        applyChoices();
        biasNow = bias.value();
        bumpNow = headBump.value();
        wearNow = wear.value();
        designHead();
        unplayed = true;
    }

    void clearState() {
        reel.start(0.0);
        takeup.start(0.25);
        capstan.start(0.0);
        idler.start(0.5);
        drift1 = 0.0;
        drift2 = 0.0;
        driftTarget = 0.0;
        scrape1 = 0.0;
        scrapeTarget = 0.0;
        scrapeLow = 0.0;
        travel = 0.0;
        readNow = center * factor;
        readWas = readNow;
        tick = 0;
        switching = 0;
        fade = 1.0;
        dropDepth = 0.0;
        dropAt = 1.0;
        dropStep = 0.0;
        dropOther = 1.0;
        peakField = 0.0;
        rounding = 0.0;
        roundRatio = 1.0;
        driveCache.at = 1e100;
        outputCache.at = 1e100;
        tapeRun = 0.0;
        rolling = false;
        quiet = 0.0;
        loopAt = 0;
        peakInL = 0.0;
        peakInR = 0.0;
        peakOutL = 0.0;
        peakOutR = 0.0;
    }

    void update() {
        hissOn = hiss.value() <= -30.0 ? 0.0 : hiss.gain();
        const int t = tape.index(), s = speed.index();
        const int nab = emphasis.index() == 0 && s != 0 ? 1 : 0;
        const bool changed = t != tapeNow || s != speedNow || nab != nabNow;
        if (unplayed) {
            // Until the first block, settings are where they are going.
            if (changed) applyChoices();
            biasNow = bias.value();
            bumpNow = headBump.value();
            wearNow = wear.value();
            designHead();
        } else if (changed && switching == 0) {
            switching = 1;
        }
    }

    // The tape, the speed and the standard: the record and playback curves,
    // the tape's constants and the transport's rates. A change while playing
    // waits for the output to fade out, then everything restarts clear.
    void applyChoices() {
        const int t = tape.index(), s = speed.index();
        const bool nab = emphasis.index() == 0 && s != 0;
        ips = speedIps(s);
        const double v = ips / 15.0;
        square = tapeSquare(t);
        field = tapeField(t) * dbToGain(speedFieldDb(s)) / kRef;
        molField = tapeField(t) * dbToGain(tapeMolDb(t));
        // Recording equalisation, its inverse on playback (the same at every
        // level, so a quiet signal comes back as it went in).
        const double fTau = 1.0 / (twoPi * speedTau(s, nab));
        const double eps = 0.316;
        const double fLow = 1.0 / (twoPi * 3180e-6);
        for (Track& k : track) {
            k.rec.design(t, s, nab, hi, false);
            k.play.design(t, s, nab, hi, true);
            // The hiss: white in the tape, through the playback curve.
            k.hissTau.design(fTau, 0.45 * rate, 1.0, rate);
            k.hissTop.design(0.49 * rate, hissCorner(s), 1.0, rate);
            if (nab)
                k.hissLow.design(fLow * eps, fLow, eps, rate);
            else
                k.hissLow.design(fLow, fLow, 1.0, rate);
        }
        tapeNow = t;
        speedNow = s;
        nabNow = nab ? 1 : 0;
        calibrateHiss(t, s);
        modGain = dbToGain(tapeModDb(t));
        // Modulation noise: a low noise of unit RMS that the signal rides.
        const double modHz = 1500.0 * v;
        modCoef = 1.0 - exp(-twoPi * modHz / rate);
        modScale = sqrt(2.0 * rate / (pi * modHz));
        // The transport's rates follow the speed: the reels turn once per
        // metre or so, the capstan (25 mm) and an idler (11 mm) faster.
        reel.rate(0.6 * v / rate);
        takeup.rate(0.5 * v / rate);
        capstan.rate(4.85 * v / rate);
        idler.rate(11.0 * v / rate);
        // The random parts: noise held for kTick frames (R values a second)
        // through low-passes, scaled to unit RMS by their noise bandwidths.
        const double held = rate / kTick;
        driftCoef = 1.0 - exp(-twoPi * 0.5 * v / rate);
        driftScale = sqrt(2.0 * held / (pi * 0.5 * v));
        scrapeCoef = 1.0 - exp(-twoPi * 25.0 * v / rate);
        scrapeLowCoef = 1.0 - exp(-twoPi * 6.0 * v / rate);
        scrapeScale = sqrt(held / (pi * 19.0 * v));
        designHead();
    }

    // What bias, the head bump and wear decide, from their gliding values.
    void designHead() {
        const int t = tapeNow, s = speedNow;
        const double b = biasNow;
        // Pinning: the recommended bias leaves almost none; underbias lets
        // it grow by 25 dB at half the current.
        const double basePin = tapePin(t) * (s >= 2 ? (s == 2 ? 3.0 : 8.0) : 1.0);
        pin = b < 1.0 ? basePin * pow(10.0, 2.5 * (1.0 - b) / 0.5)
                      : basePin * max(0.2, 1.0 - 0.6 * (b - 1.0));
        sensitivity = dbToGain(biasLevelDb(b));
        // Wear: magnetized heads (a steady field), spacing and azimuth.
        const double w = wearNow;
        dc = 0.25 * w * tapeField(t);
        dcOut = anhysteretic(dc, square);
        const double theta = w * 10.0 / 60.0 * pi / 180.0;  // up to 10 arcminutes
        azimuth = min(0.004 * tan(theta) / (ips * 0.0254), 0.45 * reach);
        dropChance = 2.0 * w * w / rate;
        // The playback head: the bump, the dip above it and the lows below,
        // where the speed puts them, the aligned top, wear and bias's tilt.
        for (Track& k : track)
            k.head.design(s, t, bumpNow, wearCorner(w, s), biasTiltDb(s, b), rate);
    }

    // Bias, the head bump and wear glide (30 ms), redesigned every kTick
    // frames while they move.
    double glideTo(double now, double target, double tiny) const {
        const double next = now + glideCoef * (target - now);
        return abs(target - next) < tiny ? target : next;
    }
    void glide() {
        if (biasNow == bias.value() && bumpNow == headBump.value() && wearNow == wear.value())
            return;
        biasNow = glideTo(biasNow, bias.value(), 1e-4);
        bumpNow = glideTo(bumpNow, headBump.value(), 1e-4);
        wearNow = glideTo(wearNow, wear.value(), 1e-4);
        designHead();
    }

    // The particles' own noise loses its shortest wavelengths too.
    double hissCorner(int s) const { return 10000.0 * speedIps(s) / 15.0; }

    // The hiss's gain for unit white noise, so that through the playback
    // curve and the aligned head it measures the tape's A-weighted figure
    // (IEC at each speed: NAB, with more lift of the highs on playback, then
    // comes out a little noisier by itself).
    void calibrateHiss(int t, int s) {
        Section tau, top;
        tau.design(1.0 / (twoPi * speedTau(s, false)), 0.45 * rate, 1.0, rate);
        top.design(0.49 * rate, hissCorner(s), 1.0, rate);
        Biquad head;
        const double topHz = speedTopHz(s) / tapeLoss(t);
        const bool headOn = topHz < 0.45 * rate;
        if (headOn) head.lowpass(topHz, 0.75);
        double power = 0.0;
        const int n = 512;
        for (int i = 1; i <= n; ++i) {
            const double f = (i - 0.5) * 0.5 * rate / n;
            double g = tau.gainAt(f, rate) * top.gainAt(f, rate) * aWeight(f);
            if (headOn) g *= dbToGain(head.responseDb(f));
            power += g * g;
        }
        power = power / n;  // unit white noise's power, A-weighted
        const double wanted = kRefRms * dbToGain(speedHissDb(s) + tapeHissDb(t) + 60.5);
        hissGain = wanted / sqrt(power);
    }

    // The transport, once per frame: where the read is, in oversampled
    // samples, and the dropouts' gain.
    void moveTape() {
        if (tick == 0) {
            driftTarget = random.gaussian();
            scrapeTarget = random.gaussian();
            glide();
            roundRatio = dbToGain(rounding);
            reel.hold();
            takeup.hold();
            capstan.hold();
            idler.hold();
        }
        tick = (tick + 1) % kTick;
        // The random parts: a slow drift, and a band for the scrape.
        drift1 += driftCoef * (driftTarget - drift1);
        drift2 += driftCoef * (drift1 - drift2);
        scrape1 += scrapeCoef * (scrapeTarget - scrape1);
        scrapeLow += scrapeLowCoef * (scrape1 - scrapeLow);
        const double drift = drift2 * driftScale, scrape = (scrape1 - scrapeLow) * scrapeScale;
        reel.next();
        takeup.next();
        capstan.next();
        idler.next();
        const double w = wear.value();
        const double wowNow = wow.value() + 0.0005 * w;
        const double flutterNow = flutter.value() + 0.0008 * w;
        const double speedError = wowNow * (0.5 * reel.s + 0.25 * takeup.s + 0.3 * drift) +
                                  flutterNow * (0.45 * capstan.s + 0.2 * idler.s + 0.35 * scrape);
        travel = travel * travelKeep + speedError / rate;
        const double limit = reach - azimuth;
        const double moved = limit > 0.0 ? limit * tanh(travel / limit) : 0.0;
        readWas = readNow;
        readNow = (center + moved * rate) * factor;
        // Dropouts: dips in the oxide's contact, more often with wear.
        if (dropAt >= 1.0 && dropChance > 0.0 && random.next() < dropChance) {
            dropAt = 0.0;
            dropDepth = 1.0 - dbToGain(-(1.0 + random.next() * (2.0 + 14.0 * w)));
            dropStep = 1.0 / ((0.008 + random.next() * 0.05) * rate);
            dropOther = 0.6 + 0.4 * random.next();
        }
    }

    // A read between samples of the ring: a cubic (Lagrange) through four.
    double readRing(const Track& t, double delay) const {
        const int n = t.ring.size();
        const int whole = floorToInt(delay);
        const double f = delay - whole;
        const int i0 = t.at - whole;
        double ym1 = 0.0, y0 = 0.0, y1 = 0.0, y2 = 0.0;
        if (i0 >= 2 && i0 + 1 < n) {
            ym1 = t.ring[i0 + 1];
            y0 = t.ring[i0];
            y1 = t.ring[i0 - 1];
            y2 = t.ring[i0 - 2];
        } else {
            ym1 = t.ring[wrap(i0 + 1, n)];
            y0 = t.ring[wrap(i0, n)];
            y1 = t.ring[wrap(i0 - 1, n)];
            y2 = t.ring[wrap(i0 - 2, n)];
        }
        if (f == 0.0) return y0;
        // Lagrange through the four, as a cubic in f (Horner).
        const double third = 1.0 / 3.0, sixth = 1.0 / 6.0;
        const double c1 = y1 - ym1 * third - y0 * 0.5 - y2 * sixth;
        const double c2 = (ym1 + y1) * 0.5 - y0;
        const double c3 = (y2 - ym1) * sixth + (y0 - y1) * 0.5;
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    void process(Block& io) {
        if (unplayed) unplayed = false;
        const int channels = min(io.channels, 2);
        double vuL = 0.0, vuR = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            moveTape();
            double drop = 0.0;
            if (dropAt < 1.0) {
                drop = dropDepth * sin(pi * dropAt);
                dropAt += dropStep;
            }
            // A switch: the output fades out; at silence the new curves go
            // in and every stage starts clear; then the input fades in, so
            // the tape fills again from nothing.
            double outFade = 1.0, inFade = 1.0;
            if (switching == 1) {
                fade = max(0.0, fade - fadeStep);
                outFade = fade;
                if (fade == 0.0) {
                    applyChoices();
                    for (Track& t : track) t.clear();
                    switching = 2;
                    inFade = 0.0;
                }
            } else if (switching == 2) {
                fade = min(1.0, fade + fadeStep);
                inFade = fade;
                if (fade == 1.0) switching = 0;
            }
            const double g = drive.at(i);
            const double driveGain = driveCache.get(g) * inFade;
            const double back = autoGain.on() ? 1.0 / driveCache.get(g) : 1.0;
            const double out = outputCache.get(output.at(i)) * back * sensitivity * outFade;
            bool loud = false;
            for (int c = 0; c < channels; ++c) {
                Track& t = track[c];
                const double x = io[c][i];
                if (abs(x) > 1e-5) loud = true;
                const double rec = x * driveGain;
                // The VU meter: full-wave, two poles of 65 ms, read as RMS.
                t.vu1 += vuCoef * (abs(rec) - t.vu1);
                t.vu2 += vuCoef * (t.vu1 - t.vu2);
                if (c == 0)
                    peakInL = max(abs(x), peakInL * fall);
                else
                    peakInR = max(abs(x), peakInR * fall);
                // Modulation noise: the signal's level rides a low noise.
                t.modZ1 += modCoef * (noise.bipolar() * 1.7320508075688772 - t.modZ1);
                t.modZ2 += modCoef * (t.modZ1 - t.modZ2);
                const double mod = 1.0 + hissOn * modGain * modScale * t.modZ2;
                const double modOver = mod / field;
                t.os.up(rec);
                const double readStep = (readNow - readWas) / factor;
                const double offset = c == 0 ? azimuth * hi : 0.0;
                for (int k = 0; k < factor; ++k) {
                    const double u = t.rec.process(t.os.sample(k));
                    const double h = u * field + dc;
                    const double m = t.oxide.step(h, square, pin);
                    const double a = abs(h - dc);
                    if (a > peakField) peakField = a;
                    // The rounding: the field against the flux, at the peaks.
                    if (a > 0.05 * molField && a > roundRatio * abs(m - dcOut)) {
                        roundRatio = a / max(abs(m - dcOut), 1e-9);
                        rounding = gainToDb(roundRatio);
                    }
                    const double y = t.play.process((m - dcOut) * modOver);
                    t.at = t.at + 1 == t.ring.size() ? 0 : t.at + 1;
                    t.ring[t.at] = y;
                    const double where = readWas + readStep * (k + 1) + offset;
                    t.os.set(k, readRing(t, where));
                    if (c == 0 && k == factor - 1) {
                        loopH[loopAt] = static_cast<float>(h);
                        loopM[loopAt] = static_cast<float>(m);
                        loopAt = loopAt + 1 == kLoop ? 0 : loopAt + 1;
                    }
                }
                double y = t.os.down();
                if (hissOn > 0.0) {
                    const double n =
                        t.hissTop.process(t.hissLow.process(t.hissTau.process(noise.gaussian())));
                    y += n * hissGain * hissOn * inFade;
                }
                y = t.head.process(y);
                const double dipped = 1.0 - drop * (c == 0 ? 1.0 : dropOther);
                y = y * out * dipped;
                io[c][i] = y;
                if (c == 0) {
                    peakOutL = max(abs(y), peakOutL * fall);
                    vuL = t.vu2;
                } else {
                    peakOutR = max(abs(y), peakOutR * fall);
                    vuR = t.vu2;
                }
            }
            if (channels == 1) {
                peakInR = peakInL;
                peakOutR = peakOutL;
                vuR = vuL;
            }
            peakField *= peakKeep;
            rounding *= roundKeep;
            if (rolling) tapeRun += ips * 0.0254 / rate;
            quiet = loud ? 0.0 : quiet + 1.0 / rate;
        }
        // The VU reads RMS for a sine: its average times pi / (2 sqrt 2).
        const double l = gainToDb(vuL * 1.1107207345395915 / kRefRms);
        const double r = gainToDb(vuR * 1.1107207345395915 / kRefRms);
        vu.set(clamp(max(l, r), -40.0, 12.0));
        headroom.set(peakField > 1e-9 ? clamp(gainToDb(molField / peakField), -36.0, 48.0) : 48.0);
        saturation.set(clamp(rounding, 0.0, 24.0));
        vuLeft.set(l);
        vuRight.set(r);
        inLeft.set(gainToDb(peakInL));
        inRight.set(gainToDb(peakInR));
        outLeft.set(gainToDb(peakOutL));
        outRight.set(gainToDb(peakOutR));
        rolling = quiet < 1.0 || io.transport.playing;
        tapeMetres.set(tapeRun);
        running.set(rolling);
        loopHead.set(loopAt);
        loopField.set(molField);
        sampleRateNow.set(rate);
    }

    // --- the editor's layout ------------------------------------------------------------------

    static constexpr double kW = 1040;
    static constexpr double kH = 760;
    // The machine's face: reels, heads and meters.
    static constexpr double fx = 16, fy = 64, fw = 1008, fh = 236;
    static constexpr double reelY = 176, reelR = 88;
    static constexpr double leftReelX = 132, rightReelX = 908;
    static constexpr double tapeY = 262;
    static constexpr double meterY = 80, meterW = 196, meterH = 142;
    static constexpr double meterLX = 316, meterRX = 528;
    // The panels below.
    static constexpr double py = 312, ph = 432;
    static constexpr double rx = 16, rw = 320;                                // record
    static constexpr double mx = 348, mw = 344;                               // magnetism
    static constexpr double bx = 704, bw = 320;                               // playback
    static constexpr double lx = 364, ly = py + 40, ls = 312;                 // the loop's well
    static constexpr double gx = bx + 16, gy = py + 124, gw = 288, gh = 104;  // the response
    static constexpr int kKnobs = 8;

    // The editor's own.
    int dragging = 0;  // 1 a knob, 2 the loop (drive and bias)
    int dragKnob = -1;
    double knobPos = 0.0;
    int hotKnob = -1;
    double hoverX = -1.0, hoverY = -1.0;
    Emphasis view;  // the record curve, for the response graph
    Head viewHead;

    void editor(Editor& e) { e.size(kW, kH); }

    // --- drawing helpers ----------------------------------------------------------------------

    void panel(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, Theme::surface, kGroundBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 10);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
        g.strokeStyle(Theme::lineLit);
        g.globalAlpha(0.5);
        g.beginPath();
        g.moveTo(x + 10, y + 0.5);
        g.lineTo(x + w - 10, y + 0.5);
        g.stroke();
        g.globalAlpha(1.0);
    }

    void well(Canvas& g, double x, double y, double w, double h) {
        g.linearGradient(0, y, 0, y + h, kGraphTop, kGraphBottom);
        g.beginPath();
        g.roundRect(x, y, w, h, 6);
        g.fill();
        g.strokeStyle(Theme::line);
        g.lineWidth(1);
        g.stroke();
    }

    void heading(Canvas& g, const char* text, double x, double y) {
        g.fillStyle(Theme::text);
        g.serifFont(14);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void aside(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.serifItalicFont(10.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void label(Canvas& g, const char* text, double x, double y, TextAlign align) {
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(align);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x, y);
    }

    void pill(Canvas& g, double x, double y, double w, double h, const char* text, bool lit,
              int colour, bool hot) {
        g.fillStyle(lit ? colour : (hot ? 0x3A2F27 : Theme::raised));
        g.beginPath();
        g.roundRect(x, y, w, h, h * 0.5);
        g.fill();
        if (!lit) {
            g.strokeStyle(Theme::line);
            g.lineWidth(1);
            g.stroke();
        }
        g.fillStyle(lit ? kInk : Theme::textMuted);
        g.boldFont(9.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(text, x + w * 0.5, y + h * 0.5 + 0.5);
    }

    bool hovering(double x, double y, double w, double h) const {
        return dragging == 0 && hoverX >= x && hoverX <= x + w && hoverY >= y && hoverY <= y + h;
    }

    // A knob: an arc from 7 to 5 o'clock, lit from `from` (a position) to the value.
    void dial(Canvas& g, double x, double y, double r, double norm, double from, int colour,
              bool hot, const char* name) {
        const double start = 0.75 * pi, sweep = 1.5 * pi;
        g.lineCap(LineCap::Round);
        g.strokeStyle(kInk);
        g.lineWidth(r > 32 ? 6 : 5);
        g.beginPath();
        g.arc(x, y, r, start, start + sweep);
        g.stroke();
        const double at = start + sweep * clamp(norm, 0.0, 1.0);
        const double base = start + sweep * clamp(from, 0.0, 1.0);
        g.strokeStyle(colour);
        g.lineWidth(r > 32 ? 4.5 : 3.5);
        if (abs(at - base) > 0.001) {
            g.beginPath();
            if (at >= base)
                g.arc(x, y, r, base, at);
            else
                g.arc(x, y, r, at, base);
            g.stroke();
        }
        g.radialGradient(x, y - r * 0.25, 2, r - 4, hot ? 0x5E4E41 : kKnobLight, kKnobDark);
        g.beginPath();
        g.circle(x, y, r - 7);
        g.fill();
        g.strokeStyle(Theme::text);
        g.lineWidth(2);
        g.beginPath();
        g.moveTo(x + cos(at) * (r - 16), y + sin(at) * (r - 16));
        g.lineTo(x + cos(at) * (r - 9), y + sin(at) * (r - 9));
        g.stroke();
        g.fillStyle(Theme::textFaint);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
        g.fillText(name, x, y - r - 12);
    }

    void valueStyle(Canvas& g, double size) {
        g.fillStyle(Theme::text);
        g.monoFont(size);
        g.textAlign(TextAlign::Center);
        g.textBaseline(TextBaseline::Middle);
    }

    // The mark: a reel of tape, its pack in umber.
    void drawMark(Canvas& g, double x, double y) {
        g.fillStyle(kOxide);
        g.beginPath();
        g.circle(x, y, 12);
        g.fill();
        g.strokeStyle(kUmber);
        g.lineWidth(1.5);
        g.beginPath();
        g.circle(x, y, 12);
        g.stroke();
        g.fillStyle(kGroundTop);
        for (int k = 0; k < 3; ++k) {
            const double a = -pi * 0.5 + k * twoPi / 3.0;
            g.beginPath();
            g.circle(x + cos(a) * 6.5, y + sin(a) * 6.5, 2.6);
            g.fill();
        }
        g.fillStyle(kUmber);
        g.beginPath();
        g.circle(x, y, 2.2);
        g.fill();
    }

    void drawLevel(Canvas& g, double x, double y, double w, double left, double right,
                   const char* name) {
        label(g, name, x - 8, y + 4, TextAlign::Right);
        for (int c = 0; c < 2; ++c) {
            const double db = c == 0 ? left : right;
            const double my = y + c * 6;
            g.fillStyle(kInk);
            g.beginPath();
            g.roundRect(x, my, w, 4, 2);
            g.fill();
            const double t = clamp((db + 60.0) / 60.0, 0.0, 1.0);
            if (t > 0.0) {
                g.linearGradient(x, 0, x + w, 0, kMeterLow, db > -0.1 ? Theme::alert : kMeterHigh);
                g.beginPath();
                g.roundRect(x, my, w * t, 4, 2);
                g.fill();
            }
        }
    }

    void drawHeader(Canvas& g) {
        drawMark(g, 31, 30);
        g.fillStyle(Theme::text);
        g.serifFont(23);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Alphabetic);
        g.fillText("Umber", 52, 37);
        const double tw = g.measureText("Umber");
        g.fillStyle(Theme::textMuted);
        g.serifItalicFont(13);
        g.fillText("tape machine", 60 + tw, 37);
        g.textBaseline(TextBaseline::Middle);
        drawLevel(g, 640, 23, 140, inLeft.get(), inRight.get(), "IN");
        drawLevel(g, 860, 23, 140, outLeft.get(), outRight.get(), "OUT");
    }

    // --- the machine's face -------------------------------------------------------------------

    // A reel: its flange with three windows, the pack of tape at its radius,
    // the hub; turned by `angle`.
    void drawReel(Canvas& g, double x, double y, double pack, double angle, const char* name) {
        g.fillStyle(kInk);
        g.globalAlpha(0.55);
        g.beginPath();
        g.circle(x + 3, y + 5, reelR);
        g.fill();
        g.globalAlpha(1.0);
        // The pack, seen through the flange.
        g.radialGradient(x, y, 20, pack, 0x5B3A25, kOxide);
        g.beginPath();
        g.circle(x, y, pack);
        g.fill();
        g.strokeStyle(0x8E5C3C);
        g.lineWidth(0.6);
        g.globalAlpha(0.45);
        for (int r = 30; r < pack; r += 7) {
            g.beginPath();
            g.circle(x, y, r);
            g.stroke();
        }
        g.globalAlpha(1.0);
        // The flange: a metal disc with windows onto the pack.
        g.save();
        g.translate(x, y);
        g.rotate(angle);
        g.fillStyle(0x6F655B);
        g.globalAlpha(0.92);
        for (int k = 0; k < 3; ++k) {
            const double a = k * twoPi / 3.0;
            g.beginPath();
            g.moveTo(cos(a - 0.26) * 22, sin(a - 0.26) * 22);
            g.arc(0, 0, reelR, a - 0.48, a + 0.48);
            g.lineTo(cos(a + 0.26) * 22, sin(a + 0.26) * 22);
            g.closePath();
            g.fill();
        }
        g.globalAlpha(1.0);
        g.strokeStyle(0x9C9083);
        g.lineWidth(1.5);
        g.beginPath();
        g.circle(0, 0, reelR);
        g.stroke();
        g.fillStyle(0x8C8073);
        g.beginPath();
        g.circle(0, 0, 22);
        g.fill();
        g.fillStyle(kInk);
        for (int k = 0; k < 3; ++k) {
            const double a = pi / 3.0 + k * twoPi / 3.0;
            g.beginPath();
            g.roundRect(cos(a) * 9 - 3, sin(a) * 9 - 3, 6, 6, 2);
            g.fill();
        }
        g.restore();
        g.fillStyle(kCream);
        g.beginPath();
        g.circle(x, y, 6);
        g.fill();
        g.fillStyle(Theme::textMuted);
        g.boldFont(8);
        g.textAlign(TextAlign::Center);
        g.fillText(name, x, y + reelR + 16);
    }

    // A head: a block the tape wraps, its gap a line at the top.
    void drawHead(Canvas& g, double x, const char* name, bool lit) {
        g.linearGradient(0, tapeY, 0, tapeY + 26, 0x8C8073, 0x4A4239);
        g.beginPath();
        g.roundRect(x - 13, tapeY + 2, 26, 22, 4);
        g.fill();
        g.fillStyle(lit ? kUmber : kInk);
        g.fillRect(x - 0.6, tapeY + 2, 1.2, 8);
        label(g, name, x, tapeY + 31, TextAlign::Center);
    }

    // A VU meter: a cream face, the scale in volts (as VU meters are), a red
    // zone above 0 VU, the needle and a peak lamp for the tape's limit.
    double vuAngle(double db) const {
        const double v = dbToGain(clamp(db, -22.0, 3.5));
        const double t = (v - 0.1) / (dbToGain(3.0) - 0.1);
        return -pi * 0.5 - 0.82 + 1.64 * t;
    }

    void drawMeter(Canvas& g, double x, double y, double needle, const char* name) {
        g.fillStyle(kInk);
        g.beginPath();
        g.roundRect(x - 4, y - 4, meterW + 8, meterH + 8, 8);
        g.fill();
        g.linearGradient(0, y, 0, y + meterH, kCream, 0xD9CBB0);
        g.beginPath();
        g.roundRect(x, y, meterW, meterH, 5);
        g.fill();
        const double cx = x + meterW * 0.5, cy = y + meterH + 34, r = 132;
        g.save();
        g.clip(x, y, meterW, meterH);
        // The red zone.
        g.strokeStyle(0xB8452A);
        g.lineWidth(5);
        g.beginPath();
        g.arc(cx, cy, r - 3, vuAngle(0.0), vuAngle(3.0));
        g.stroke();
        g.strokeStyle(0x3B2F26);
        g.lineWidth(1.2);
        g.beginPath();
        g.arc(cx, cy, r, vuAngle(-20.0), vuAngle(0.0));
        g.stroke();
        Array<double, 11> ticks = {-20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3};
        for (int k = 0; k < 11; ++k) {
            const double a = vuAngle(ticks[k]);
            const bool red = ticks[k] > 0.0;
            g.strokeStyle(red ? 0xB8452A : 0x3B2F26);
            g.lineWidth(1.2);
            g.beginPath();
            g.moveTo(cx + cos(a) * r, cy + sin(a) * r);
            g.lineTo(cx + cos(a) * (r + 8), cy + sin(a) * (r + 8));
            g.stroke();
            g.fillStyle(red ? 0xB8452A : 0x3B2F26);
            g.font(9);
            g.textAlign(TextAlign::Center);
            g.textBaseline(TextBaseline::Middle);
            const double tx = cx + cos(a) * (r + 17), ty = cy + sin(a) * (r + 17);
            if (ticks[k] == 0.0)
                g.fillText("0", tx, ty);
            else if (ticks[k] > 0.0)
                g.fillText(format("+%.0f", ticks[k]), tx, ty);
            else if (ticks[k] != -1.0 && ticks[k] != -2.0)
                g.fillText(format("%.0f", -ticks[k]), tx, ty);
        }
        g.fillStyle(0x3B2F26);
        g.serifItalicFont(15);
        g.fillText("VU", cx, y + meterH - 38);
        g.boldFont(8);
        g.fillText(name, cx, y + meterH - 20);
        // The needle.
        const double a = vuAngle(needle);
        g.strokeStyle(0x1E1813);
        g.lineWidth(1.4);
        g.beginPath();
        g.moveTo(cx + cos(a) * 40, cy + sin(a) * 40);
        g.lineTo(cx + cos(a) * (r + 6), cy + sin(a) * (r + 6));
        g.stroke();
        g.restore();
        // The pivot's cover.
        g.fillStyle(0x3B2F26);
        g.fillRect(x, y + meterH - 10, meterW, 10);
        g.fillStyle(0x2A221C);
        g.fillRect(x, y + meterH - 10, meterW, 1);
    }

    void drawFace(Canvas& g) {
        panel(g, fx, fy, fw, fh);
        const double metres = tapeMetres.get();
        // The reels: a 30-minute reel's tape, wound from left to right.
        const double whole = 686.0, hub = 26.0, full = 80.0;
        const double u = fmod(metres, whole) / whole;
        const double left = sqrt(full * full - (full * full - hub * hub) * u);
        const double right = sqrt(hub * hub + (full * full - hub * hub) * u);
        // Each turns at the tape's speed over its pack's radius (a pack of 80
        // drawn units standing for 13 cm).
        const double scale = 0.13 / full;
        const double leftAngle = metres / (left * scale);
        const double rightAngle = metres / (right * scale);
        // The tape's path: off the left pack, round a guide, over the heads,
        // between the capstan and its roller, round a guide, onto the right.
        g.strokeStyle(kOxide);
        g.lineWidth(3);
        g.lineCap(LineCap::Round);
        g.beginPath();
        g.moveTo(leftReelX - left * 0.2, reelY + left * 0.98);
        g.lineTo(236, tapeY - 6);
        g.lineTo(640, tapeY - 2);
        g.lineTo(760, tapeY - 2);
        g.lineTo(806, tapeY - 6);
        g.lineTo(rightReelX + right * 0.2, reelY + right * 0.98);
        g.stroke();
        drawReel(g, leftReelX, reelY, left, -leftAngle, "SUPPLY");
        drawReel(g, rightReelX, reelY, right, -rightAngle, "TAKE-UP");
        // Guides, the heads, the capstan and its pinch roller.
        g.fillStyle(0x8C8073);
        g.beginPath();
        g.circle(236, tapeY - 12, 7);
        g.fill();
        g.beginPath();
        g.circle(806, tapeY - 12, 7);
        g.fill();
        const bool lit = running.get();
        drawHead(g, 330, "ERASE", false);
        drawHead(g, 420, "RECORD", lit);
        drawHead(g, 510, "PLAY", lit);
        g.fillStyle(0xB5AA9C);
        g.beginPath();
        g.circle(700, tapeY - 9, 7);
        g.fill();
        g.fillStyle(0x2E2620);
        g.beginPath();
        g.circle(700, tapeY + 9, 11);
        g.fill();
        label(g, "CAPSTAN", 700, tapeY + 31, TextAlign::Center);
        // The settings, as a tape box's label would say them.
        g.fillStyle(Theme::textMuted);
        g.monoFont(10);
        g.textAlign(TextAlign::Left);
        g.textBaseline(TextBaseline::Middle);
        const int s = speed.index();
        const char* sp = "15 ips";
        if (s == 0) sp = "30 ips";
        if (s == 2) sp = "7.5 ips";
        if (s == 3) sp = "3.75 ips";
        const char* eq = emphasis.index() == 0 ? "NAB" : "IEC";
        if (s == 0) eq = "AES";
        g.fillText(sp, 744, 160);
        g.fillText(eq, 744, 176);
        g.fillStyle(Theme::textFaint);
        g.fillText(latency.index() == 0 ? "full" : "low latency", 744, 192);
        // The meters (their ballistics are the audio code's).
        drawMeter(g, meterLX, meterY, vuLeft.get(), "LEFT");
        drawMeter(g, meterRX, meterY, vuRight.get(), "RIGHT");
        // The peak lamp: the loudest recent peak over the tape's 3% limit.
        const bool over = headroom.value() < 0.0;
        g.fillStyle(over ? Theme::alert : 0x3A2B22);
        g.beginPath();
        g.circle(meterRX + meterW + 24, meterY + 12, 5);
        g.fill();
        if (over) {
            g.globalAlpha(0.3);
            g.beginPath();
            g.circle(meterRX + meterW + 24, meterY + 12, 10);
            g.fill();
            g.globalAlpha(1.0);
        }
        label(g, "PEAK", meterRX + meterW + 24, meterY + 28, TextAlign::Center);
    }

    // --- the knobs ----------------------------------------------------------------------------

    // 0 drive, 1 bias, 2 output, 3 head bump, 4 wow, 5 flutter, 6 wear, 7 hiss
    double knobX(int k) const {
        if (k == 0) return rx + 86;
        if (k == 1) return rx + 238;
        if (k == 2) return rx + 238;
        if (k == 3) return bx + 60;
        if (k == 4) return bx + 160;
        if (k == 5) return bx + 260;
        if (k == 6) return bx + 110;
        return bx + 210;
    }
    double knobY(int k) const {
        if (k < 2) return py + 150;
        if (k == 2) return py + 372;
        if (k < 6) return py + 286;
        return py + 378;
    }
    double knobR(int k) const {
        if (k == 0) return 46;
        if (k == 1) return 34;
        if (k == 2) return 28;
        return 26;
    }
    double knobValue(int k) const {
        if (k == 0) return drive.normalized();
        if (k == 1) return bias.normalized();
        if (k == 2) return output.normalized();
        if (k == 3) return headBump.normalized();
        if (k == 4) return wow.normalized();
        if (k == 5) return flutter.normalized();
        if (k == 6) return wear.normalized();
        return hiss.normalized();
    }
    double knobFrom(int k) const {
        if (k == 0 || k == 1 || k == 2) return 0.5;
        if (k == 3) return 0.5;
        return 0.0;
    }

    void drawKnob(Canvas& g, int k) {
        const char* name = "DRIVE";
        if (k == 1) name = "BIAS";
        if (k == 2) name = "OUTPUT";
        if (k == 3) name = "HEAD BUMP";
        if (k == 4) name = "WOW";
        if (k == 5) name = "FLUTTER";
        if (k == 6) name = "WEAR";
        if (k == 7) name = "HISS";
        int colour = kUmber;
        if (k == 1 && bias.value() < 0.995) colour = kRust;
        if (k == 1 && bias.value() > 1.005) colour = kMoss;
        if (k == 7 && hiss.value() <= -30.0) colour = 0x6B5A48;
        dial(g, knobX(k), knobY(k), knobR(k), knobValue(k), knobFrom(k), colour, hotKnob == k,
             name);
        valueStyle(g, k == 0 ? 12 : 10.5);
        const double x = knobX(k), y = knobY(k) + knobR(k) + 13;
        if (k == 0) {
            if (abs(drive.value()) < 0.05)
                g.fillText("0.0 dB", x, y);
            else
                g.fillText(format("%+.1f dB", drive.value()), x, y);
        } else if (k == 1) {
            g.fillText(format("%.0f%%", bias.value() * 100.0), x, y);
        } else if (k == 2) {
            if (abs(output.value()) < 0.05)
                g.fillText("0.0 dB", x, y);
            else
                g.fillText(format("%+.1f dB", output.value()), x, y);
        } else if (k == 3) {
            g.fillText(format("%.0f%%", headBump.value() * 100.0), x, y);
        } else if (k == 4) {
            g.fillText(format("%.2f%%", wow.value() * 100.0), x, y);
        } else if (k == 5) {
            g.fillText(format("%.2f%%", flutter.value() * 100.0), x, y);
        } else if (k == 6) {
            g.fillText(format("%.0f%%", wear.value() * 100.0), x, y);
        } else {
            if (hiss.value() <= -30.0)
                g.fillText("off", x, y);
            else if (abs(hiss.value()) < 0.5)
                g.fillText("real", x, y);
            else
                g.fillText(format("%+.0f dB", hiss.value()), x, y);
        }
    }

    // --- record -------------------------------------------------------------------------------

    void drawRecord(Canvas& g) {
        panel(g, rx, py, rw, ph);
        heading(g, "Record", rx + 16, py + 18);
        aside(g, "how hard, onto what", rx + rw - 16, py + 18, TextAlign::Right);
        const int t = tape.index();
        for (int k = 0; k < 3; ++k) {
            const double x = rx + 16 + k * 98;
            const char* name = "MODERN";
            if (k == 1) name = "CLASSIC";
            if (k == 2) name = "VINTAGE";
            pill(g, x, py + 40, 92, 24, name, t == k, kUmber, hovering(x, py + 40, 92, 24));
        }
        drawKnob(g, 0);
        drawKnob(g, 1);
        const double b = bias.value();
        const char* note = "recommended";
        if (b < 0.995) note = b < 0.8 ? "underbiased: gritty" : "under: brighter";
        if (b > 1.005) note = b > 1.2 ? "overbiased: dull" : "over: darker, cleaner";
        aside(g, note, knobX(1), knobY(1) + 62, TextAlign::Center);
        // Readouts: the headroom left and how much the peaks were rounded.
        const double ty = py + 246;
        for (int k = 0; k < 2; ++k) {
            const double x = rx + 16 + k * 148;
            g.fillStyle(Theme::recess);
            g.beginPath();
            g.roundRect(x, ty, 140, 52, 6);
            g.fill();
            label(g, k == 0 ? "HEADROOM" : "PEAKS ROUNDED", x + 12, ty + 14, TextAlign::Left);
            g.fillStyle(Theme::text);
            g.monoFont(16);
            g.textAlign(TextAlign::Left);
            g.textBaseline(TextBaseline::Middle);
            if (k == 0) {
                const double h = headroom.value();
                if (h >= 47.9)
                    g.fillText("-", x + 12, ty + 35);
                else {
                    g.fillStyle(h < 0.0 ? Theme::alert : Theme::text);
                    g.fillText(format("%+.1f dB", h), x + 12, ty + 35);
                }
            } else {
                g.fillText(format("%.2f dB", saturation.value()), x + 12, ty + 35);
            }
        }
        aside(g, "to the tape's 3% limit, and taken off", rx + rw * 0.5, ty + 66,
              TextAlign::Center);
        pill(g, rx + 28, py + 360, 116, 24, "AUTO GAIN", autoGain.on(), kUmber,
             hovering(rx + 28, py + 360, 116, 24));
        drawKnob(g, 2);
    }

    // --- magnetism ----------------------------------------------------------------------------

    // The trace's scale: the field at the tape's limit sits at 0.62 of the
    // half-width, so the knee and some saturation show.
    double loopX(double h, double scale) const { return lx + ls * 0.5 + h / scale * ls * 0.31; }
    double loopY(double m) const { return ly + ls * 0.5 - m * ls * 0.44; }

    void drawLoop(Canvas& g) {
        panel(g, mx, py, mw, ph);
        heading(g, "Magnetism", mx + 16, py + 18);
        aside(g, "the oxide's flux against the field", mx + mw - 16, py + 18, TextAlign::Right);
        well(g, lx, ly, ls, ls);
        const double scale = max(loopField.get(), 1e-6);
        g.save();
        g.clip(lx, ly, ls, ls);
        // Axes, the reference level and the 3% limit.
        g.strokeStyle(kGrid);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(lx, ly + ls * 0.5);
        g.lineTo(lx + ls, ly + ls * 0.5);
        g.moveTo(lx + ls * 0.5, ly);
        g.lineTo(lx + ls * 0.5, ly + ls);
        g.stroke();
        const int t = tape.index();
        const double ref = tapeField(t), mol = tapeField(t) * dbToGain(tapeMolDb(t));
        for (int side = -1; side <= 1; side += 2) {
            g.strokeStyle(kGridLit);
            g.beginPath();
            g.moveTo(loopX(side * ref, scale), ly + 8);
            g.lineTo(loopX(side * ref, scale), ly + ls - 8);
            g.stroke();
            g.strokeStyle(kRust);
            g.globalAlpha(0.6);
            g.beginPath();
            g.moveTo(loopX(side * mol, scale), ly + 8);
            g.lineTo(loopX(side * mol, scale), ly + ls - 8);
            g.stroke();
            g.globalAlpha(1.0);
        }
        // The anhysteretic curve: where the bias leads the oxide.
        const double sq = tapeSquare(t);
        g.strokeStyle(kGridLit);
        g.lineWidth(1.5);
        g.beginPath();
        for (int i = 0; i <= 120; ++i) {
            const double h = (i / 60.0 - 1.0) * scale * 1.62;
            const double x = loopX(h, scale), y = loopY(anhysteretic(h, sq));
            if (i == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        // The trace, newest brightest.
        const int head = loopHead.get();
        g.strokeStyle(kUmber);
        g.lineWidth(1.6);
        g.lineCap(LineCap::Round);
        for (int seg = 0; seg < 8; ++seg) {
            g.globalAlpha(0.15 + 0.85 * (seg + 1) / 8.0);
            g.beginPath();
            for (int j = 0; j <= kLoop / 8; ++j) {
                const int idx = wrap(head + seg * (kLoop / 8) + j, kLoop);
                const double x = loopX(loopH[idx], scale), y = loopY(loopM[idx]);
                if (j == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        g.globalAlpha(1.0);
        g.restore();
        label(g, "0 VU", loopX(ref, scale) + 4, ly + 14, TextAlign::Left);
        g.fillStyle(kRust);
        g.boldFont(8.5);
        g.textAlign(TextAlign::Left);
        g.fillText("3%", loopX(mol, scale) + 4, ly + 14);
        label(g, "FIELD", lx + ls - 8, ly + ls * 0.5 + 10, TextAlign::Right);
        label(g, "FLUX", lx + ls * 0.5 - 6, ly + 12, TextAlign::Right);
        const double b = bias.value();
        const char* about = "biased: the oxide follows the curve, and only the peaks bend";
        if (b < 0.9) about = "underbiased: the loop opens, and quiet sounds turn grainy";
        if (b > 1.1) about = "overbiased: cleaner, with less top";
        aside(g, about, mx + mw * 0.5, ly + ls + 22, TextAlign::Center);
        aside(g, "drag: across for drive, up and down for bias", mx + mw * 0.5, ly + ls + 42,
              TextAlign::Center);
    }

    // --- playback -----------------------------------------------------------------------------

    // The fundamental's gain of a sine of amplitude a through the curve.
    double describe(double a, double sq) const {
        if (a < 1e-6) return 1.0;
        double sum = 0.0;
        for (int k = 0; k < 24; ++k) {
            const double th = (k + 0.5) * pi * 0.5 / 24.0;
            sum += anhysteretic(a * sin(th), sq) * sin(th);
        }
        return sum * (pi * 0.5 / 24.0) * 4.0 / (pi * a);
    }

    double responseY(double db) const { return dbToY(db, -12.0, 6.0, gy, gh); }

    void drawResponse(Canvas& g) {
        well(g, gx, gy, gw, gh);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.frequencyGrid(gx, gy, gw, gh, 20, 20000);
        g.strokeStyle(kGridLit);
        g.lineWidth(1);
        g.beginPath();
        g.moveTo(gx, responseY(0.0));
        g.lineTo(gx + gw, responseY(0.0));
        g.stroke();
        const int t = tape.index(), s = speed.index();
        const bool nab = emphasis.index() == 0 && s != 0;
        const double r = sampleRateNow.get() > 1000.0 ? sampleRateNow.get() : 48000.0;
        const double hiRate = r * (latency.index() == 0 ? 4.0 : 2.0);
        view.design(t, s, nab, hiRate, false);
        viewHead.design(s, t, headBump.value(), wearCorner(wear.value(), s),
                        biasTiltDb(s, bias.value()), r);
        const double sq = tapeSquare(t);
        const double field = tapeField(t) * dbToGain(speedFieldDb(s));
        const double levelDb = biasLevelDb(bias.value());
        for (int pass = 0; pass < 2; ++pass) {
            // Quiet (-20 VU), then hot (+10 VU): the highs, lifted into the
            // tape, saturate first.
            const double amp = field * dbToGain(pass == 0 ? -20.0 : 10.0);
            g.strokeStyle(pass == 0 ? Theme::textMuted : kUmber);
            g.lineWidth(pass == 0 ? 1.4 : 2.0);
            g.beginPath();
            for (int i = 0; i <= 96; ++i) {
                const double hz = 20.0 * pow(1000.0, i / 96.0);
                const double e = view.gainAt(hz, hiRate);
                const double db =
                    viewHead.responseDb(hz) + levelDb + gainToDb(describe(amp * e, sq));
                const double x = hzToX(hz, 20, 20000, gx, gw), y = responseY(db);
                if (i == 0)
                    g.moveTo(x, y);
                else
                    g.lineTo(x, y);
            }
            g.stroke();
        }
        g.restore();
        g.fillStyle(Theme::textMuted);
        g.boldFont(8);
        g.textAlign(TextAlign::Right);
        g.textBaseline(TextBaseline::Middle);
        g.fillText("-20 VU", gx + gw - 52, gy + 10);
        g.fillStyle(kUmber);
        g.fillText("+10 VU", gx + gw - 8, gy + 10);
    }

    void drawPlayback(Canvas& g) {
        panel(g, bx, py, bw, ph);
        heading(g, "Playback", bx + 16, py + 18);
        aside(g, "the speed, the head, the transport", bx + bw - 16, py + 18, TextAlign::Right);
        const int s = speed.index();
        label(g, "SPEED", bx + 16, py + 40, TextAlign::Left);
        for (int k = 0; k < 4; ++k) {
            const double x = bx + 16 + k * 74;
            const char* name = "30 IPS";
            if (k == 1) name = "15 IPS";
            if (k == 2) name = "7.5 IPS";
            if (k == 3) name = "3.75 IPS";
            pill(g, x, py + 48, 68, 22, name, s == k, kUmber, hovering(x, py + 48, 68, 22));
        }
        label(g, "EMPHASIS", bx + 16, py + 84, TextAlign::Left);
        label(g, "LATENCY", bx + 164, py + 84, TextAlign::Left);
        g.globalAlpha(s == 0 ? 0.45 : 1.0);
        pill(g, bx + 16, py + 92, 68, 22, "NAB", emphasis.index() == 0 && s != 0, kUmber,
             s != 0 && hovering(bx + 16, py + 92, 68, 22));
        pill(g, bx + 90, py + 92, 68, 22, s == 0 ? "AES" : "IEC", emphasis.index() == 1 || s == 0,
             kUmber, s != 0 && hovering(bx + 90, py + 92, 68, 22));
        g.globalAlpha(1.0);
        pill(g, bx + 164, py + 92, 68, 22, "FULL", latency.index() == 0, kMoss,
             hovering(bx + 164, py + 92, 68, 22));
        pill(g, bx + 238, py + 92, 68, 22, "LOW", latency.index() == 1, kMoss,
             hovering(bx + 238, py + 92, 68, 22));
        drawResponse(g);
        for (int k = 3; k < kKnobs; ++k) drawKnob(g, k);
    }

    void draw(Canvas& g) {
        g.linearGradient(0, 0, 0, kH, kGroundTop, kGroundBottom);
        g.fillRect(0, 0, kW, kH);
        drawHeader(g);
        drawFace(g);
        drawRecord(g);
        drawLoop(g);
        drawPlayback(g);
    }

    // --- the pointer --------------------------------------------------------------------------

    // how: 1 a knob position (0..1), 2 the default.
    void setKnob(int k, int how, double v) {
        if (k == 0) {
            if (how == 1)
                drive.editNormalized(v);
            else
                drive.reset();
        }
        if (k == 1) {
            if (how == 1)
                bias.editNormalized(v);
            else
                bias.reset();
        }
        if (k == 2) {
            if (how == 1)
                output.editNormalized(v);
            else
                output.reset();
        }
        if (k == 3) {
            if (how == 1)
                headBump.editNormalized(v);
            else
                headBump.reset();
        }
        if (k == 4) {
            if (how == 1)
                wow.editNormalized(v);
            else
                wow.reset();
        }
        if (k == 5) {
            if (how == 1)
                flutter.editNormalized(v);
            else
                flutter.reset();
        }
        if (k == 6) {
            if (how == 1)
                wear.editNormalized(v);
            else
                wear.reset();
        }
        if (k == 7) {
            if (how == 1)
                hiss.editNormalized(v);
            else
                hiss.reset();
        }
    }

    int knobAt(double x, double y) const {
        for (int k = 0; k < kKnobs; ++k) {
            const double dx = x - knobX(k), dy = y - knobY(k);
            const double r = knobR(k) + 6;
            if (dx * dx + dy * dy <= r * r) return k;
        }
        return -1;
    }

    void pointer(const Pointer& p) {
        const PointerAction a = p.action();
        const double fine = p.shift() ? 0.2 : 1.0;
        if (a == PointerAction::Leave) {
            hoverX = -1.0;
            hoverY = -1.0;
            hotKnob = -1;
            return;
        }
        hoverX = p.x();
        hoverY = p.y();
        if (a == PointerAction::Move) {
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Press) {
            pressed(p);
            return;
        }
        if (a == PointerAction::Drag) {
            if (dragging == 1) {
                knobPos = clamp(knobPos - p.dy() * fine / 220.0, 0.0, 1.0);
                setKnob(dragKnob, 1, knobPos);
            } else if (dragging == 2) {
                drive.edit(drive.value() + p.dx() * fine * 0.12);
                bias.edit(bias.value() - p.dy() * fine * 0.003);
            }
            return;
        }
        if (a == PointerAction::Release) {
            dragging = 0;
            hotKnob = knobAt(p.x(), p.y());
            return;
        }
        if (a == PointerAction::Wheel) {
            const double notches = p.wheel();
            if (p.over(lx, ly, ls, ls)) {
                drive.edit(drive.value() + notches * 0.5 * fine);
                return;
            }
            const int k = knobAt(p.x(), p.y());
            if (k >= 0) setKnob(k, 1, clamp(knobValue(k) + notches * 0.02 * fine, 0.0, 1.0));
        }
    }

    void toggleOf(const Param& t) { t.edit(t.on() ? 0 : 1); }

    void pressed(const Pointer& p) {
        const double x = p.x(), y = p.y();
        const int k = knobAt(x, y);
        if (k >= 0) {
            if (p.doubleClick()) {
                setKnob(k, 2, 0);
                return;
            }
            dragging = 1;
            dragKnob = k;
            knobPos = knobValue(k);
            return;
        }
        if (p.over(lx, ly, ls, ls)) {
            if (p.doubleClick()) {
                drive.reset();
                bias.reset();
            } else {
                dragging = 2;
            }
            return;
        }
        for (int m = 0; m < 3; ++m)
            if (p.over(rx + 16 + m * 98, py + 40, 92, 24)) {
                tape.edit(m);
                return;
            }
        if (p.over(rx + 28, py + 360, 116, 24)) {
            toggleOf(autoGain);
            return;
        }
        for (int m = 0; m < 4; ++m)
            if (p.over(bx + 16 + m * 74, py + 48, 68, 22)) {
                speed.edit(m);
                return;
            }
        if (speed.index() != 0) {
            if (p.over(bx + 16, py + 92, 68, 22)) emphasis.edit(0);
            if (p.over(bx + 90, py + 92, 68, 22)) emphasis.edit(1);
        }
        if (p.over(bx + 164, py + 92, 68, 22)) latency.edit(0);
        if (p.over(bx + 238, py + 92, 68, 22)) latency.edit(1);
    }
};
