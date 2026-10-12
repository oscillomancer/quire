#include <supple/script.h>

// Saturation with three shapes and a tone control after it.
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
    double pre = 1.0, post = 1.0;
    Shape mode = Shape::Soft;

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
            for (int i = 0; i < io.frames; ++i) {
                const double dry = io[c][i];
                const double wet = lp.lowpass(shapeSample(dry * pre)) * post;
                io[c][i] = dry + (wet - dry) * mix;
            }
        }
    }
};
