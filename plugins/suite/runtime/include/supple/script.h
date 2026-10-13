// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The DSP script runtime: everything a script can name.
//
// A script is one C++ file that starts with `#include <supple/script.h>`.
// The script compiler accepts a checked subset of C++20 and JIT-compiles it;
// the same file also compiles as ordinary C++ against this header (a source
// export), and means the same thing. The language reference is
// skill/guides/dsp-language.md.
//
// This header deliberately includes no standard math header: every function
// here is the runtime's own, so unqualified names (`sin`, `min`, `Biquad`)
// mean the runtime's in a script whether it is read by the script compiler,
// by clangd, or by a C++ compiler in an export. Built-in objects are opaque
// storage whose methods are compiled once, in the runtime library; the JIT
// calls the very same functions, so both paths compute the same bits.
#pragma once

#include <initializer_list>
#include <type_traits>

namespace supple::script {

// --- numbers -------------------------------------------------------------------

// Math works in double: one overload each, so a call never depends on which
// overload C++ would pick (sin(1) and min(x, 0.5) with a float x would be
// ambiguous otherwise). A float argument is promoted, and the result narrows
// back when assigned to a float. min, max, clamp and abs also take ints.

inline constexpr double pi = 3.14159265358979323846;
inline constexpr double twoPi = 6.28318530717958647692;

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);  // half away from zero
double fmod(double x, double y);
double hypot(double x, double y);

inline double abs(double x) { return x < 0.0 ? -x : x; }
inline int abs(int x) { return x < 0 ? -x : x; }
inline double min(double a, double b) { return b < a ? b : a; }
inline int min(int a, int b) { return b < a ? b : a; }
inline double max(double a, double b) { return a < b ? b : a; }
inline int max(int a, int b) { return a < b ? b : a; }
inline double clamp(double x, double lo, double hi) { return x < lo ? lo : (hi < x ? hi : x); }
inline int clamp(int x, int lo, int hi) { return x < lo ? lo : (hi < x ? hi : x); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
// -1, 0 or +1.
inline double sign(double x) { return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0); }
// True unless x is NaN or infinite.
inline bool isFinite(double x) { return x - x == 0.0; }

// Decibels and gain, pitch and frequency.
double dbToGain(double db);    // 0 dB is exactly 1
double gainToDb(double gain);  // floored at -120 dB (0 gives -120)
double mtof(double note);      // MIDI note to Hz: 69 is 440 Hz
double ftom(double hz);        // Hz to MIDI note (fractional)

// A downward compressor's curve: the gain change in dB (0 or less) for a
// level in dB. Nothing below the threshold; above it, `ratio` dB in for each
// dB out (1 changes nothing, 100 limits), with a soft knee `kneeDb` wide
// (0: a hard knee) centred on the threshold.
double compressorGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb);

// Float to int, defined for every input: truncated toward zero, rounded down,
// rounded up or rounded half away from zero; saturated to the int range;
// NaN gives 0. C++'s own conversion is undefined out of range, so scripts
// convert only through these.
int toInt(double x);
int floorToInt(double x);
int ceilToInt(double x);
int roundToInt(double x);
// Integer division and remainder for any divisor: 0 when b is 0, and
// INT_MIN / -1 is INT_MIN. Scripts divide by a variable only through these.
int idiv(int a, int b);
int imod(int a, int b);
// i wrapped into 0..n-1 (for n > 0; 0 otherwise), negative i included.
int wrap(int i, int n);

// --- containers ----------------------------------------------------------------

namespace detail {
struct Layout;
}

// A fixed-size array. Indexing is checked: an index outside 0..N-1 refers to
// a spare element instead (for numbers it reads 0, and what is written to it
// is lost), and the script's status reports the line. It starts zeroed, or
// from a list: Array<float, 4> taps = {0.1f, 0.2f, 0.3f, 0.4f};
template <class T, int N>
struct Array {
    static_assert(N > 0, "an Array needs at least one element");

    T& operator[](int i) {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(N)) return items_[i];
        if constexpr (std::is_arithmetic_v<T>) spare_ = T();
        return spare_;
    }
    const T& operator[](int i) const { return const_cast<Array&>(*this)[i]; }
    static constexpr int size() { return N; }
    // Every element to `value`.
    void fill(const T& value) {
        for (int i = 0; i < N; ++i) items_[i] = value;
    }
    // For range-based for: for (auto& v : voices).
    T* begin() { return items_; }
    T* end() { return items_ + N; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + N; }

    // Public so that an Array is an aggregate and takes a list; a script
    // reaches its elements only through [] (the compiler checks).
    T items_[N] = {};
    T spare_ = {};
};

// A run-time sized buffer of numbers: resize() it in prepare() (it allocates),
// then index it like an Array. New elements are zero.
template <class T>
class Buffer {
    static_assert(std::is_arithmetic_v<T>, "a Buffer holds numbers (double, float, int)");

  public:
    Buffer() = default;
    Buffer(const Buffer& other) { *this = other; }
    Buffer& operator=(const Buffer& other);
    ~Buffer();

    void resize(int size);  // prepare() only
    int size() const { return size_; }
    T& operator[](int i) {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(size_)) return items_[i];
        spare_ = T();
        return spare_;
    }
    const T& operator[](int i) const { return const_cast<Buffer&>(*this)[i]; }
    void fill(T value) {
        for (int i = 0; i < size_; ++i) items_[i] = value;
    }
    T* begin() { return items_; }
    T* end() { return items_ + size_; }
    const T* begin() const { return items_; }
    const T* end() const { return items_ + size_; }

  private:
    friend struct detail::Layout;
    T* items_ = nullptr;
    int size_ = 0;
    T spare_ = T();
};

// --- parameters ----------------------------------------------------------------

// The physical unit of a parameter. The patch carries values in it, hosts
// show it, and agents set it.
enum class Unit {
    None,
    Decibels,
    Hertz,
    Milliseconds,
    Percent,
    Ratio,
    Semitones,
    Cents,
    Octaves,
    Degrees,
    Bpm,
    Samples
};

// How a parameter's value maps onto a knob's travel and a host's 0..1
// automation: Linear, Log (equal ratios per unit of travel), or Pow (exponent
// k, favouring the low end when k > 1).
enum class Curve { Linear, Log, Pow };

// A parameter's description, made by the factories below and refined by
// chained modifiers: decibels("Gain", -24, 24, 0).smoothed(20).help("...").
struct ParamSpec {
    enum class Kind { Continuous, Integer, Choice, Toggle };
    Kind kind = Kind::Continuous;
    const char* name = "";
    double min = 0.0, max = 1.0, def = 0.0;
    Unit unit = Unit::None;
    Curve curve = Curve::Linear;
    double curveK = 1.0;
    std::initializer_list<const char*> choices = {};
    const char* group = "";
    const char* description = "";
    const char* key = "";      // the patch key; the member's name when empty
    unsigned int id = 0;       // the CLAP id; a hash of the key when 0
    double smoothingMs = 0.0;  // the framework glides it; read it with at(i)
    bool automatable = true, modulatable = false, offAtMinimum = false, output = false;
    bool bypass = false, hidden = false;
    std::initializer_list<const char*> roles = {};  // "level", "time", "brightness" ...

    // In a group, which the automatic editor and hosts show as a section.
    ParamSpec in(const char* g) const {
        ParamSpec p = *this;
        p.group = g;
        return p;
    }
    // What it does, in one sentence: the editor's tooltip and the schema's description.
    ParamSpec help(const char* text) const {
        ParamSpec p = *this;
        p.description = text;
        return p;
    }
    // The framework glides it over `ms`, per sample; read param.at(i).
    ParamSpec smoothed(double ms) const {
        ParamSpec p = *this;
        p.smoothingMs = ms;
        return p;
    }
    ParamSpec modulated() const {
        ParamSpec p = *this;
        p.modulatable = true;
        return p;
    }
    // Its minimum means off ("-inf dB" for a level).
    ParamSpec offAtMin() const {
        ParamSpec p = *this;
        p.offAtMinimum = true;
        return p;
    }
    // A meter the plugin reports with set(): read-only, never saved.
    ParamSpec meter() const {
        ParamSpec p = *this;
        p.output = true;
        p.automatable = false;
        return p;
    }
    ParamSpec notAutomatable() const {
        ParamSpec p = *this;
        p.automatable = false;
        return p;
    }
    ParamSpec withKey(const char* k) const {
        ParamSpec p = *this;
        p.key = k;
        return p;
    }
    ParamSpec withId(unsigned int i) const {
        ParamSpec p = *this;
        p.id = i;
        return p;
    }
    ParamSpec role(std::initializer_list<const char*> r) const {
        ParamSpec p = *this;
        p.roles = r;
        return p;
    }
    ParamSpec curved(Curve c, double k = 1.0) const {
        ParamSpec p = *this;
        p.curve = c;
        p.curveK = k;
        return p;
    }
};

inline ParamSpec continuous(const char* name, double min, double max, double def,
                            Unit unit = Unit::None) {
    ParamSpec p;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    return p;
}
// A level or gain in dB, linear travel.
inline ParamSpec decibels(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Decibels);
}
// A frequency in Hz, logarithmic travel.
inline ParamSpec frequency(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Hertz).curved(Curve::Log);
}
// A time in ms: logarithmic travel when min > 0, otherwise a cubic curve.
inline ParamSpec milliseconds(const char* name, double min, double max, double def) {
    ParamSpec p = continuous(name, min, max, def, Unit::Milliseconds);
    return min > 0.0 ? p.curved(Curve::Log) : p.curved(Curve::Pow, 3.0);
}
// A percentage given in percent (0..100 by default); the script reads it as a
// fraction, 0..1.
inline ParamSpec percent(const char* name, double def, double min = 0.0, double max = 100.0) {
    return continuous(name, min, max, def, Unit::Percent);
}
// A ratio such as a compressor's 4:1, logarithmic travel.
inline ParamSpec ratio(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Ratio).curved(Curve::Log);
}
inline ParamSpec semitones(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Semitones);
}
inline ParamSpec cents(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Cents);
}
inline ParamSpec octaves(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Octaves);
}
inline ParamSpec degrees(const char* name, double min, double max, double def) {
    return continuous(name, min, max, def, Unit::Degrees);
}
// A whole number.
inline ParamSpec integer(const char* name, int min, int max, int def) {
    ParamSpec p = continuous(name, min, max, def);
    p.kind = ParamSpec::Kind::Integer;
    return p;
}
// One of several options, read as an index: choice("Mode", {"Lowpass", "Highpass"}, 0).
inline ParamSpec choice(const char* name, std::initializer_list<const char*> options, int def) {
    ParamSpec p = continuous(name, 0.0, static_cast<double>(options.size()) - 1.0, def);
    p.kind = ParamSpec::Kind::Choice;
    p.choices = options;
    return p;
}
// On or off.
inline ParamSpec toggle(const char* name, bool def) {
    ParamSpec p = continuous(name, 0.0, 1.0, def ? 1.0 : 0.0);
    p.kind = ParamSpec::Kind::Toggle;
    return p;
}

namespace detail {
struct ParamBinding;
struct Instance;
}  // namespace detail

// A parameter: a member of the plugin, read as a number in its unit (a
// percent as 0..1, a choice as its index, a toggle as 0 or 1). Its value is
// the host's at the start of the current block (blocks are split at every
// change, so automation is sample-accurate). A smoothed() one also glides:
// at(i) is its value at frame i of the block.
class Param {
  public:
    Param(const ParamSpec& spec);  // registers it with the plugin under construction
    Param(const Param& other);
    Param& operator=(const Param& other);

    operator double() const { return value_; }
    double value() const { return value_; }
    // The value at frame i of the current block: glided for a smoothed()
    // parameter, the block's value otherwise.
    double at(int i) const {
        if (glide_ && static_cast<unsigned>(i) < static_cast<unsigned>(glideFrames_))
            return glide_[i];
        return value_;
    }
    // A choice's index, a toggle's state, a level in dB as a linear gain
    // (exactly 0 at the minimum of an offAtMin() one).
    int index() const { return roundToInt(value_); }
    bool on() const { return value_ > 0.5; }
    double gain() const;
    // In update(): whether this parameter changed since the last update.
    bool changed() const { return changed_; }
    // A meter() parameter: what the plugin reports.
    void set(double value) { reported_ = value; }

  private:
    friend struct detail::ParamBinding;
    friend struct detail::Layout;
    double value_ = 0.0;
    const double* glide_ = nullptr;
    int glideFrames_ = 0;
    bool changed_ = true;
    bool offAtMinimum_ = false;
    double min_ = 0.0;
    double reported_ = 0.0;
    int slot_ = -1;
};

// --- files ---------------------------------------------------------------------

// A file member as the script declares it: audioFile("Sample") or
// impulseResponse("Room"), with .help() and .withKey() as a parameter has.
struct FileSpec {
    enum class Kind { Audio, Impulse };
    Kind kind = Kind::Audio;
    const char* name = "";
    const char* description = "";
    const char* key = "";  // the patch key; the member's name when empty

    FileSpec help(const char* text) const {
        FileSpec f = *this;
        f.description = text;
        return f;
    }
    FileSpec withKey(const char* k) const {
        FileSpec f = *this;
        f.key = k;
        return f;
    }
};
// A file of audio -- WAV, AIFF or FLAC -- for an AudioFile: a sample, a loop,
// a wavetable.
inline FileSpec audioFile(const char* name) {
    FileSpec f;
    f.name = name;
    return f;
}
// An impulse response -- WAV, AIFF or FLAC -- for a Convolver: a room, a
// speaker cabinet.
inline FileSpec impulseResponse(const char* name) {
    FileSpec f;
    f.kind = FileSpec::Kind::Impulse;
    f.name = name;
    return f;
}

namespace detail {
struct FileContents;
struct FileBinding;
}  // namespace detail

// A file of audio: a member of the plugin struct, AudioFile sample =
// audioFile("Sample"). Its path is a property of the plugin, saved with it
// and set by the user or a patch ("properties": {"sample": "/path.wav"}).
// The file is loaded off the audio thread and resampled to the session's
// rate; it appears at the start of a block, and loaded() is false until
// then, or when there is none.
class AudioFile {
  public:
    AudioFile(const FileSpec& spec);  // registers it with the plugin under construction
    bool loaded() const;
    int channels() const;  // 0 when nothing is loaded
    int frames() const;
    // Sample `frame` of `channel`, 0 outside the file. A channel past the
    // file's last reads its last, so a mono file serves every channel.
    double sample(int channel, int frame) const;
    // Between samples, interpolated (cubic): for playing at another speed.
    double read(int channel, double frame) const;

  private:
    friend struct detail::FileBinding;
    detail::FileContents* contents_ = nullptr;
};

// Convolution with an impulse response: a member of the plugin struct,
// Convolver room = impulseResponse("Room"), whose path is a property as an
// AudioFile's is. Loaded and prepared off the audio thread (its first 20
// seconds); no latency. Until one is loaded, process() gives 0.
class Convolver {
  public:
    Convolver(const FileSpec& spec);  // registers it with the plugin under construction
    bool loaded() const;
    int length() const;  // the response's frames
    // The next output of channel c for input x. Each channel uses the
    // response's channel of the same number, or its last.
    double process(int channel, double x);

  private:
    friend struct detail::FileBinding;
    detail::FileContents* contents_ = nullptr;
};

// --- the plugin ----------------------------------------------------------------

// The plugin's identity and buses, `static constexpr Info info = {...}` in the
// plugin struct. The ID never changes once the plugin is shared.
struct Info {
    const char* name = "";
    const char* id = "";  // reverse-DNS: "com.example.tilt"
    const char* vendor = "";
    const char* version = "1.0.0";
    const char* description = "";
    const char* url = "";
    // Main bus widths offered, the first the default: {2, 1} is stereo, then
    // mono. 0 entries means stereo and mono for an effect, stereo otherwise.
    int channels[4] = {0, 0, 0, 0};
    bool sidechain = false;       // a sidechain input as wide as the main bus
    const char* outputs[8] = {};  // names of extra outputs (an instrument's "Kick", "Snare")
    int voices = 0;               // an instrument's voice pool, for hosts that ask
    int latency = 0;              // samples, reported to the host
    int tail = 0;                 // samples of output after the input stops; -1 forever
};

// Plain read-only views of one channel of audio.
class InputChannel {
  public:
    double operator[](int i) const {
        return static_cast<unsigned>(i) < static_cast<unsigned>(frames_) ? data_[i] : 0.0;
    }
    int size() const { return frames_; }

    const double* data_ = nullptr;
    int frames_ = 0;
};

class Channel {
  public:
    double& operator[](int i) const {
        if (static_cast<unsigned>(i) < static_cast<unsigned>(frames_)) return data_[i];
        *spare_ = 0.0;
        return *spare_;
    }
    int size() const { return frames_; }

    double* data_ = nullptr;
    int frames_ = 0;
    double* spare_ = nullptr;
};

// The host's transport at the start of the block.
struct Transport {
    bool playing = false;
    double tempo = 120.0;  // beats per minute (120 when the host sent none)
    double beats = 0.0;    // song position in quarter notes
    double barStart = 0.0;
    int timeSigNumerator = 4, timeSigDenominator = 4;
};

// One block of audio, processed in place: io[channel][frame]. For an effect
// it holds the input on entry and must hold the output on return; for an
// instrument it arrives silent. A block may be a single frame long.
class Block {
  public:
    int channels = 0;
    int frames = 0;
    Transport transport;
    bool sidechainConnected = false;

    Channel operator[](int c) const {
        if (static_cast<unsigned>(c) < static_cast<unsigned>(channels))
            return Channel{data_[c], frames, spare_};
        return Channel{spare_, 0, spare_};
    }
    // The sidechain (Info::sidechain): the key input, silent when unconnected.
    InputChannel sidechain(int c) const {
        if (static_cast<unsigned>(c) < static_cast<unsigned>(sidechainChannels_))
            return InputChannel{sidechain_[c], frames};
        return InputChannel{nullptr, 0};
    }
    // An extra output (Info::outputs), silent on entry.
    Channel output(int bus, int c) const {
        if (static_cast<unsigned>(bus) < static_cast<unsigned>(outputs_) &&
            static_cast<unsigned>(c) < static_cast<unsigned>(channels))
            return Channel{aux_[bus][c], frames, spare_};
        return Channel{spare_, 0, spare_};
    }

    double* const* data_ = nullptr;
    const double* const* sidechain_ = nullptr;
    int sidechainChannels_ = 0;
    double* const* const* aux_ = nullptr;
    int outputs_ = 0;
    double* spare_ = nullptr;
};

// What every plugin struct derives from, through Effect, Instrument or
// NoteEffect.
class Plugin {
  public:
    double sampleRate() const;
    int maxBlockSize() const;
    // The main bus width chosen by the host.
    int channels() const;
    // Whether the host renders offline (a bounce).
    bool offline() const;
    // In prepare(): the delay the plugin adds, in samples, when it depends
    // on the sample rate (an oversampler's, a lookahead); Info's .latency
    // otherwise. The host compensates it.
    void setLatency(int samples);

  private:
    friend struct detail::Instance;
    friend struct detail::Layout;
    friend class Instrument;
    detail::Instance* instance_ = nullptr;
};

// Audio in, audio out.
class Effect : public Plugin {};

// How an instrument's voices take notes. Poly gives each note a voice of its
// own. Mono plays the newest key held on voice 0, and returns to the last one
// still held when it is released. Legato is Mono gliding from note to note
// without starting the voice again.
enum class VoiceMode { Poly, Mono, Legato };

// Notes in, audio out. The runtime hands each note to one of the
// instrument's voices (Info's .voices, 8 when it gives none), stealing the
// oldest when every voice is busy, and holds released notes while the
// sustain pedal is down. It calls the instrument's
//
//   void startVoice(int v)     voice v starts a note: voiceKey(v), voiceVelocity(v)
//   void releaseVoice(int v)   its key is up: start the release
//   void stopVoice(int v)      (optional) silence it at once: stolen or choked
//
// process() adds every voice that voicePlaying(v) into io, which arrives
// silent, and calls voiceFinished(v) once a voice's release has died away,
// which tells the host its note is over. startVoice may take a second
// argument, startVoice(int v, bool retrigger): false in Legato mode, for a
// note that glides from the one sounding, whose envelope keeps going.
class Instrument : public Plugin {
  public:
    int voiceCount() const;
    bool voicePlaying(int v) const;     // from startVoice until voiceFinished
    int voiceKey(int v) const;          // the note's MIDI key, 0..127
    int voiceChannel(int v) const;      // its MIDI channel, 0..15
    double voiceVelocity(int v) const;  // 0..1
    // At frame i of the block: the voice's pitch in semitones (its key,
    // gliding, plus its channel's pitch bend), and its frequency in Hz.
    double voicePitch(int v, int i) const;
    double voiceHz(int v, int i) const;
    void voiceFinished(int v);
    // Usually in update(): how notes take voices, the glide from one note to
    // the next (Mono and Legato), and the pitch bend's reach in semitones (2
    // unless set).
    void setVoiceMode(VoiceMode mode);
    void setGlide(double ms);
    void setBendRange(double semitones);
};

// Notes in, notes out. Not available to scripts yet, and the compiler says
// so.
class NoteEffect : public Plugin {};

// --- DSP -------------------------------------------------------------------------

// Every object below works at the plugin's sample rate. Their state starts
// silent, and clear() returns it there.

// A biquad filter (RBJ cookbook designs). q is the resonance (0.707 is
// Butterworth); for the shelves it sets the slope.
class Biquad {
  public:
    Biquad();
    void lowpass(double hz, double q);
    void highpass(double hz, double q);
    void bandpass(double hz, double q);  // 0 dB at the centre
    void notch(double hz, double q);
    void allpass(double hz, double q);
    void peak(double hz, double q, double gainDb);
    void lowShelf(double hz, double q, double gainDb);
    void highShelf(double hz, double q, double gainDb);
    double process(double x);
    void clear();
    // The design's gain at hz, in dB (for drawing a response).
    double responseDb(double hz) const;

  private:
    alignas(8) unsigned char storage_[56];
};

// A one-pole filter with low-pass and high-pass outputs.
class OnePole {
  public:
    OnePole();
    void setCutoff(double hz);
    double lowpass(double x);
    double highpass(double x);
    void clear();

  private:
    alignas(8) unsigned char storage_[24];
};

// A compressor's detector: the signal's level (rectified) through a one-pole
// that rises at the attack time and falls at the release time. Pair it with
// compressorGainDb:
//
//   double level = follower.process(key);
//   y = x * dbToGain(compressorGainDb(gainToDb(level), threshold, ratio, 6));
class EnvelopeFollower {
  public:
    EnvelopeFollower();
    void set(double attackMs, double releaseMs);
    double process(double x);  // the level now, linear
    double level() const;
    void clear();

  private:
    alignas(8) unsigned char storage_[24];
};

// A delay line with fractional (Hermite) reads. prepare() its longest delay
// in prepare(); push() one sample per frame, read() any delay up to it.
class DelayLine {
  public:
    DelayLine();
    DelayLine(const DelayLine& other);
    DelayLine& operator=(const DelayLine& other);
    ~DelayLine();
    void prepare(int maxDelaySamples);  // prepare() only: it allocates
    void push(double x);
    double read(double delaySamples) const;  // 0 is the sample just pushed
    void clear();

  private:
    void* line_ = nullptr;
};

// Oversampling, for anything nonlinear: a shaper's harmonics above half the
// sample rate fold back as aliases unless they are made at a higher rate and
// filtered away coming down. One per channel, per sample:
//
//   o.up(x);
//   for (int k = 0; k < o.factor(); ++k) o.set(k, tanh(drive * o.sample(k)));
//   y = o.down();
//
// prepare(factor) in prepare(): 2, 4, 8 or 16 (until then it passes samples
// through, at 1). Its filters are minimum phase: latency() is a few samples
// of delay, which setLatency() reports.
class Oversampler {
  public:
    Oversampler();
    Oversampler(const Oversampler& other);
    Oversampler& operator=(const Oversampler& other);
    ~Oversampler();
    void prepare(int factor);  // prepare() only: it allocates
    int factor() const;
    int latency() const;
    void up(double x);
    double sample(int k) const;  // 0 .. factor() - 1
    void set(int k, double value);
    double down();
    void clear();

  private:
    void* state_ = nullptr;
};

// The short-time Fourier transform, for spectral effects and analysis: one
// per channel. Each sample, push() the input; when it returns true a frame's
// spectrum is ready, bins 0 to bins() - 1, to read and change; pull() then
// gives the output, latency() samples behind the input:
//
//   if (s.push(x))
//       for (int k = 0; k < s.bins(); ++k) s.scale(k, gainAt(s.binHz(k)));
//   y = s.pull();
//
// prepare(size, overlap) in prepare(): size a power of two from 16 to 32768
// (rounded down), overlap 4 or 8 frames over each sample. Frames are Hann
// windowed both ways, so a spectrum left alone gives back the input. Until
// prepared it passes samples through. The bins are readable and writable
// from a push() that returned true until the pull() after it; otherwise
// they read 0 and writes are lost. A sine of amplitude 1 at a bin's centre
// frequency reads magnitude 1 there.
class Stft {
  public:
    Stft();
    Stft(const Stft& other);
    Stft& operator=(const Stft& other);
    ~Stft();
    void prepare(int size, int overlap);  // prepare() only: it allocates
    int size() const;
    int bins() const;  // size() / 2 + 1
    int hop() const;   // samples between frames: size() / overlap
    int latency() const;
    double binHz(int k) const;  // bin k's centre frequency
    bool push(double x);
    double pull();
    double real(int k) const;
    double imag(int k) const;
    double magnitude(int k) const;
    double phase(int k) const;  // radians
    void set(int k, double real, double imag);
    void setPolar(int k, double magnitude, double phase);
    void scale(int k, double gain);
    void clear();

  private:
    void* state_ = nullptr;
};

// Pseudo-random numbers (PCG32), the same sequence everywhere for a seed.
class Random {
  public:
    Random();
    void seed(int seed);
    double next();     // 0 <= x < 1
    double bipolar();  // -1 <= x < 1
    double gaussian();
    int below(int n);  // 0 .. n-1

  private:
    alignas(8) unsigned char storage_[16];
};

// White, pink and brown noise, about -10 dBFS RMS.
class Noise {
  public:
    Noise();
    void seed(int seed);
    double white();
    double pink();
    double brown();

  private:
    alignas(8) unsigned char storage_[80];
};

// An anti-aliased (polyBLEP) oscillator.
class Oscillator {
  public:
    enum class Shape { Sine, Saw, Square, Triangle };
    Oscillator();
    void setShape(Shape shape);
    void setFrequency(double hz);
    void setPulseWidth(double width);  // square: 0.01..0.99
    void setPhase(double phase);       // 0..1
    double phase() const;
    double next();

  private:
    alignas(8) unsigned char storage_[48];
};

// An ADSR envelope: times in ms, sustain 0..1. gate(true) starts it,
// gate(false) releases it; active() is false once it has died away.
class Adsr {
  public:
    Adsr();
    void set(double attackMs, double decayMs, double sustain, double releaseMs);
    void setCurves(double attack, double decayRelease);  // 0 linear .. 1 most curved
    void gate(bool on);
    void retrigger();
    void kill();
    double next();
    double level() const;
    bool active() const;

  private:
    alignas(8) unsigned char storage_[192];
};

}  // namespace supple::script

using namespace supple::script;
