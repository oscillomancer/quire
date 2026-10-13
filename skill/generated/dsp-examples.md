# DSP scripts: the examples

Generated from `plugins/suite/examples` by `tools/gen/update-generated.sh`.
Every one is tested: compiled to machine code, interpreted and compiled as
C++, with the same output, and played by the script player. Start from the
closest, change it, and keep what works (`guides/dsp-language.md` is the
language, `guides/dsp-workflow.md` the commands).

| Example | What it is |
|---|---|
| [gain](#gain) | The smallest useful effect: a level in dB, glided so that moving it does not click. |
| [tilt](#tilt) | A tilt equaliser: one control raises the highs and lowers the lows around a pivot (or the reverse), with two shelves meeting at the pivot. |
| [compressor](#compressor) | A compressor that can listen to another track. With a signal on its sidechain input (a kick drum, a voice), that signal decides how far the main one is turned down: ducking. With none, it listens to its own input. |
| [echo](#echo) | A feedback delay: what comes back is fed in again, quieter each time. |
| [drive](#drive) | Saturation with three shapes and a tone control after it. The shaper runs at four times the sample rate, so its harmonics alias far less; the dry signal is delayed to match, and the delay is reported to the host. |
| [spectral](#spectral) | Three effects in the frequency domain: a gate that keeps only the parts of the spectrum above a threshold (a crude noise reduction), a robot voice (every phase zeroed: each frame repeats as a buzz at the hop rate) and a whisper (every phase random: pitch dissolves into breath). |
| [reverb](#reverb) | A convolution reverb: the input played through a recording of a space's echo (an impulse response), after a pre-delay, with a tone control on the reverb and a dry/wet mix. |
| [synth](#synth) | A polyphonic synth: a saw per voice through a low-pass filter and an envelope. With Glide above 0 it plays one note at a time, sliding. |
| [sampler](#sampler) | A sampler: every key plays the sample, faster up the keyboard and slower down it; the root key plays it as recorded. Each voice reads the file at its own speed under an envelope, and ends with the sample. |
| [meter](#meter) | A gain with an editor of its own: a scope of the output and a level meter beside a knob. The audio code fills Shared members; the editor draws them. |

## gain

```cpp
#include <supple/script.h>

// The smallest useful effect: a level in dB, glided so that moving it does
// not click.
struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain", .vendor = "Example"};

    Param gain = decibels("Gain", -60, 24, 0).smoothed(20).help("Level change, in dB.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
    }
};
```

## tilt

```cpp
#include <supple/script.h>

// A tilt equaliser: one control raises the highs and lowers the lows around
// a pivot (or the reverse), with two shelves meeting at the pivot.
struct Tilt : Effect {
    static constexpr Info info = {.name = "Tilt", .id = "com.example.tilt", .vendor = "Example"};

    Param tilt = decibels("Tilt", -6, 6, 0).help("Positive brightens: highs up, lows down.");
    Param pivot = frequency("Pivot", 200, 5000, 1000).help("Where the two shelves meet.");
    Param output = decibels("Output", -12, 12, 0).smoothed(20);

    Array<Biquad, 2> low, high;

    void update() {
        for (int c = 0; c < 2; ++c) {
            low[c].lowShelf(pivot, 0.707, -tilt * 0.5);
            high[c].highShelf(pivot, 0.707, tilt * 0.5);
        }
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c) {
            for (int i = 0; i < io.frames; ++i) {
                double x = high[c].process(low[c].process(io[c][i]));
                io[c][i] = x * dbToGain(output.at(i));
            }
        }
    }
};
```

## compressor

```cpp
#include <supple/script.h>

// A compressor that can listen to another track. With a signal on its
// sidechain input (a kick drum, a voice), that signal decides how far the
// main one is turned down: ducking. With none, it listens to its own input.
struct Compressor : Effect {
    static constexpr Info info = {.name = "Compressor",
                                  .id = "com.example.compressor",
                                  .vendor = "Example",
                                  .sidechain = true};

    Param threshold = decibels("Threshold", -60, 0, -24).help("Where turning down begins.");
    // A member may not share a factory's name, so the key is given.
    Param slope = ratio("Ratio", 1, 20, 4).withKey("ratio").help("dB in for each dB out above it.");
    Param attack = milliseconds("Attack", 0.1, 100, 10).help("How fast it turns down.");
    Param release = milliseconds("Release", 10, 2000, 150).help("How fast it lets go.");
    Param makeup = decibels("Makeup", 0, 24, 0).smoothed(20).help("Gain after compressing.");
    Param reduction = decibels("Reduction", -40, 0, 0).meter().help("How far it turns down now.");

    EnvelopeFollower follower;

    void update() { follower.set(attack, release); }

    void process(Block& io) {
        double most = 0.0;
        for (int i = 0; i < io.frames; ++i) {
            // The louder channel sets the level, so the stereo image holds still.
            double peak = 0.0;
            for (int c = 0; c < io.channels; ++c)
                peak = max(peak, abs(io.sidechainConnected ? io.sidechain(c)[i] : io[c][i]));
            const double level = follower.process(peak);
            const double changeDb = compressorGainDb(gainToDb(level), threshold, slope, 6.0);
            const double gain = dbToGain(changeDb + makeup.at(i));
            for (int c = 0; c < io.channels; ++c) io[c][i] *= gain;
            most = min(most, changeDb);
        }
        reduction.set(max(most, -40.0));
    }
};
```

## echo

```cpp
#include <supple/script.h>

// A feedback delay: what comes back is fed in again, quieter each time.
struct Echo : Effect {
    static constexpr Info info = {
        .name = "Echo", .id = "com.example.echo", .vendor = "Example", .tail = -1};

    Param time = milliseconds("Time", 1, 2000, 350).help("Time between repeats.");
    Param feedback = percent("Feedback", 40, 0, 95).help("How much of each repeat returns.");
    Param mix = percent("Mix", 30).help("Dry to wet.");

    Array<DelayLine, 2> lines;

    void prepare() {
        // Two seconds at any sample rate, allocated here, never in process().
        for (auto& line : lines) line.prepare(toInt(sampleRate() * 2.0) + 4);
    }

    void process(Block& io) {
        const double delay = time * 0.001 * sampleRate();
        for (int c = 0; c < io.channels && c < 2; ++c) {
            DelayLine& line = lines[c];
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                const double wet = line.read(delay);
                line.push(dry + wet * feedback);
                io[c][i] = lerp(dry, wet, mix);
            }
        }
    }
};
```

## drive

```cpp
#include <supple/script.h>

// Saturation with three shapes and a tone control after it. The shaper runs
// at four times the sample rate, so its harmonics alias far less; the dry
// signal is delayed to match, and the delay is reported to the host.
enum class Shape { Soft, Hard, Fold };

// Folds anything beyond +-1 back into range, as many times as it takes.
double fold(double x) {
    int guard = 0;
    while ((x > 1.0 || x < -1.0) && guard < 64) {
        x = x > 1.0 ? 2.0 - x : -2.0 - x;
        ++guard;
    }
    return x;
}

struct Drive : Effect {
    static constexpr Info info = {.name = "Drive", .id = "com.example.drive", .vendor = "Example"};

    Param drive = decibels("Drive", 0, 36, 12).help("Gain into the shaper.");
    Param shape = choice("Shape", {"Soft", "Hard", "Fold"}, 0);
    Param tone = frequency("Tone", 500, 18000, 8000).help("Low-pass after the shaper.");
    Param mix = percent("Mix", 100);

    Array<OnePole, 2> filters;
    Array<Oversampler, 2> over;
    Array<DelayLine, 2> dryDelay;
    double pre = 1.0, post = 1.0;
    Shape mode = Shape::Soft;

    void prepare() {
        for (int c = 0; c < 2; ++c) {
            over[c].prepare(4);
            dryDelay[c].prepare(64);
        }
        setLatency(over[0].latency());
    }

    void update() {
        pre = dbToGain(drive);
        post = 1.0 / sqrt(pre);
        mode = static_cast<Shape>(shape.index());
        for (auto& f : filters) f.setCutoff(tone);
    }

    double shapeSample(double x) const {
        switch (mode) {
            case Shape::Soft:
                return tanh(x);
            case Shape::Hard:
                return clamp(x, -1.0, 1.0);
            case Shape::Fold:
                return fold(x);
        }
        return x;
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels && c < 2; ++c) {
            OnePole& lp = filters[c];
            Oversampler& o = over[c];
            DelayLine& d = dryDelay[c];
            const double lag = o.latency();
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                o.up(dry * pre);
                for (int k = 0; k < o.factor(); ++k) o.set(k, shapeSample(o.sample(k)));
                const double wet = lp.lowpass(o.down()) * post;
                d.push(dry);
                const double aligned = d.read(lag);
                io[c][i] = aligned + (wet - aligned) * mix;
            }
        }
    }
};
```

## spectral

```cpp
#include <supple/script.h>

// Three effects in the frequency domain: a gate that keeps only the parts of
// the spectrum above a threshold (a crude noise reduction), a robot voice
// (every phase zeroed: each frame repeats as a buzz at the hop rate) and a
// whisper (every phase random: pitch dissolves into breath).
enum class Mode { Gate, Robot, Whisper };

struct Spectral : Effect {
    static constexpr Info info = {
        .name = "Spectral", .id = "com.example.spectral", .vendor = "Example"};

    Param mode = choice("Mode", {"Gate", "Robot", "Whisper"}, 0);
    Param threshold =
        decibels("Threshold", -100, 0, -60).help("Gate: frequencies quieter than this go.");
    Param mix = percent("Mix", 100).smoothed(20);

    Array<Stft, 2> stft;
    Array<DelayLine, 2> dry;  // the input, delayed as much as the transform delays
    Random random;
    Mode effect = Mode::Gate;

    void prepare() {
        for (int c = 0; c < 2; ++c) {
            stft[c].prepare(1024, 4);
            dry[c].prepare(1024);
        }
        setLatency(stft[0].latency());
        random.seed(7);
    }

    void update() { effect = static_cast<Mode>(mode.index()); }

    // Changes one frame's spectrum.
    void change(Stft& s) {
        for (int k = 0; k < s.bins(); ++k) {
            switch (effect) {
                case Mode::Gate:
                    if (gainToDb(s.magnitude(k)) < threshold) s.scale(k, 0.0);
                    break;
                case Mode::Robot:
                    s.setPolar(k, s.magnitude(k), 0.0);
                    break;
                case Mode::Whisper:
                    s.setPolar(k, s.magnitude(k), random.next() * twoPi);
                    break;
            }
        }
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels && c < 2; ++c) {
            Stft& s = stft[c];
            DelayLine& d = dry[c];
            for (int i = 0; i < io.frames; ++i) {
                const double x = io[c][i];
                if (s.push(x)) change(s);
                const double wet = s.pull();
                d.push(x);
                io[c][i] = lerp(d.read(s.latency()), wet, mix.at(i));
            }
        }
    }
};
```

## reverb

```cpp
#include <supple/script.h>

// A convolution reverb: the input played through a recording of a space's
// echo (an impulse response), after a pre-delay, with a tone control on the
// reverb and a dry/wet mix.
struct Reverb : Effect {
    // The tail: the 20 seconds of response a Convolver keeps, at up to 192 kHz.
    static constexpr Info info = {.name = "Convolution Reverb",
                                  .id = "com.example.convolution-reverb",
                                  .vendor = "Example",
                                  .tail = 3840000};

    Convolver room = impulseResponse("Room").help("A recording of a space's echo.");
    Param predelay = milliseconds("Pre-Delay", 0, 200, 10).help("Time before the reverb starts.");
    Param tone = frequency("Tone", 1000, 20000, 8000).help("A low-pass on the reverb.");
    Param mix = percent("Mix", 30).smoothed(20);

    Array<DelayLine, 2> pre;
    Array<OnePole, 2> damp;

    void prepare() {
        for (DelayLine& d : pre) d.prepare(toInt(sampleRate() * 0.2) + 4);
    }

    void update() {
        for (OnePole& f : damp) f.setCutoff(tone);
    }

    void process(Block& io) {
        const double delay = predelay * 0.001 * sampleRate();
        for (int c = 0; c < io.channels && c < 2; ++c)
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                pre[c].push(dry);
                const double wet = damp[c].lowpass(room.process(c, pre[c].read(delay)));
                io[c][i] = lerp(dry, wet, mix.at(i));
            }
    }
};
```

## synth

```cpp
#include <supple/script.h>

// A polyphonic synth: a saw per voice through a low-pass filter and an
// envelope. With Glide above 0 it plays one note at a time, sliding.
struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 8};

    Param attack = milliseconds("Attack", 1, 2000, 5).help("How fast a note rises.");
    Param release =
        milliseconds("Release", 5, 5000, 300).help("How long a note fades once its key is up.");
    Param cutoff = frequency("Cutoff", 200, 16000, 3000).help("The low-pass filter's corner.");
    Param glide = milliseconds("Glide", 0, 1000, 0)
                      .help("The slide from note to note; above 0, one note at a time.");
    Param level = decibels("Level", -48, 0, -12).smoothed(20).help("The output level.");

    struct Voice {
        Oscillator osc;
        Adsr env;
        Biquad filter;
    };
    Array<Voice, 8> voice;

    void prepare() {
        for (Voice& v : voice) v.osc.setShape(Oscillator::Shape::Saw);
    }

    void update() {
        for (Voice& v : voice) {
            v.env.set(attack, 200, 0.7, release);
            v.filter.lowpass(cutoff, 0.707);
        }
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
    }

    // Voice v starts a note. retrigger is false when it glides on from the
    // note before (Legato): its envelope keeps going.
    void startVoice(int v, bool retrigger) {
        if (retrigger) voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                s.osc.setFrequency(voiceHz(v, i));
                const double tone = s.filter.process(s.osc.next());
                const double x = tone * s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!s.env.active()) voiceFinished(v);
        }
    }
};
```

## sampler

```cpp
#include <supple/script.h>

// A sampler: every key plays the sample, faster up the keyboard and slower
// down it; the root key plays it as recorded. Each voice reads the file at
// its own speed under an envelope, and ends with the sample.
struct Sampler : Instrument {
    static constexpr Info info = {
        .name = "Sampler", .id = "com.example.sampler", .vendor = "Example", .voices = 16};

    AudioFile sample = audioFile("Sample").help("The sound every key plays.");
    Param root = integer("Root Key", 0, 127, 60).help("The key that plays it as recorded.");
    Param attack = milliseconds("Attack", 0.5, 2000, 2).help("How fast a note rises.");
    Param release = milliseconds("Release", 5, 5000, 200).help("How fast it fades once let go.");
    Param level = decibels("Level", -48, 6, 0).smoothed(20).help("The output level.");

    struct Voice {
        double position = 0.0;  // in the file's frames
        Adsr env;
    };
    Array<Voice, 16> voice;

    void update() {
        for (Voice& v : voice) v.env.set(attack, 0, 1, release);
    }

    void startVoice(int v) {
        voice[v].position = 0.0;
        voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }

    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            Voice& s = voice[v];
            const double velocity = voiceVelocity(v);
            for (int i = 0; i < io.frames; ++i) {
                const double speed = exp2((voicePitch(v, i) - root) / 12.0);
                const double gain = s.env.next() * velocity * dbToGain(level.at(i));
                for (int c = 0; c < io.channels; ++c) io[c][i] += sample.read(c, s.position) * gain;
                s.position += speed;
            }
            if (!s.env.active() || s.position >= sample.frames()) voiceFinished(v);
        }
    }
};
```

## meter

```cpp
#include <supple/script.h>

// A gain with an editor of its own: a scope of the output and a level meter
// beside a knob. The audio code fills Shared members; the editor draws them.
struct Meter : Effect {
    static constexpr Info info = {.name = "Meter", .id = "com.example.meter", .vendor = "Example"};

    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level out.");

    // What the editor sees: the output's peak in dB, falling slowly, and its
    // last 256 samples (every fourth one), oldest at `head`.
    Shared<double> level;
    SharedArray<float, 256> scope;
    Shared<int> head;

    // The audio code's own.
    double peak = 0.0, fall = 0.0;
    int skip = 0;

    void prepare() { fall = exp(-1.0 / (0.3 * sampleRate())); }

    void process(Block& io) {
        int at = head.get();
        for (int i = 0; i < io.frames; ++i) {
            const double g = dbToGain(gain.at(i));
            double loudest = 0.0;
            for (int c = 0; c < io.channels; ++c) {
                io[c][i] *= g;
                loudest = max(loudest, abs(io[c][i]));
            }
            peak = max(loudest, peak * fall);
            if (++skip == 4) {
                skip = 0;
                scope[at] = static_cast<float>(io[0][i]);
                at = (at + 1) % 256;
            }
        }
        head.set(at);
        level.set(gainToDb(peak));
    }

    void editor(Editor& e) {
        e.size(440, 260);
        e.knob(gain, 336, 140, 88, 100);
    }

    void draw(Canvas& g) {
        g.panel(16, 16, 304, 228, "Output");
        g.well(32, 56, 272, 172);
        g.strokeStyle(Theme::line);
        g.beginPath();
        g.moveTo(32, 142);
        g.lineTo(304, 142);
        g.stroke();
        g.strokeStyle(Theme::accent);
        g.lineWidth(1.5);
        g.beginPath();
        for (int k = 0; k < 256; ++k) {
            const double x = 40 + k;
            const double y =
                142 - 80 * clamp(static_cast<double>(scope[wrap(head.get() + k, 256)]), -1.0, 1.0);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();

        g.meter(level.get(), 368, 24, 24, 96);
        g.monoFont(11);
        g.textAlign(TextAlign::Center);
        g.fillStyle(level.get() > 0 ? Theme::alert : Theme::textMuted);
        g.fillText(level.get() < -100 ? "-inf" : format("%.1f dB", level.get()), 380, 134);
    }
};
```
