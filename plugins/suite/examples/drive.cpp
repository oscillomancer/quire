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

    void factoryPatches(Patches& p) {
        p.add("Gentle warmth").set(drive, 6).set(shape, "soft").set(tone, 12000).set(mix, 100);
        p.add("Parallel crunch").set(drive, 18).set(shape, "hard").set(tone, 6000).set(mix, 35);
        p.add("Folded texture").set(drive, 24).set(shape, "fold").set(tone, 4000).set(mix, 50);
    }

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
