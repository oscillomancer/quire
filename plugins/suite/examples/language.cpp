#include <supple/script.h>

// Not a useful plugin: a tour of the language, whose output the tests compare
// between the JIT, the interpreter and the same file compiled as C++.
constexpr int kTaps = 8;
constexpr double kScale = 0.25;

enum class Mode { Add, Multiply };

struct Accumulator {
    double sum = 0.0;
    int count = 0;
    float last = 0.5f;

    void add(double x) {
        sum += x;
        ++count;
        last = static_cast<float>(x);
    }
    double mean() const { return count > 0 ? sum / count : 0.0; }
};

// Integer arithmetic wraps, as exports build it (-fwrapv).
int scramble(int a, int b) { return (a * 1103515245 + b) ^ (a >> 3); }

double average(const Array<double, 8>& values) {
    double total = 0.0;
    for (double v : values) total += v;
    return total / values.size();
}

void fillRamp(Array<double, 8>& out, double start) {
    for (int i = 0; i < out.size(); ++i) out[i] = start + i * kScale;
}

struct Tour : Effect {
    static constexpr Info info = {.name = "Tour", .id = "com.example.tour"};
    static constexpr float kHalf = 0.5f;

    Param amount = percent("Amount", 50);
    Param steps = integer("Steps", 1, 16, 4);
    Param mode = choice("Mode", {"Add", "Multiply"}, 0);
    Param on = toggle("On", true);

    Array<double, kTaps> taps = {1.0, 0.5, 0.25, 0.125};
    Array<Accumulator, 2> stats;
    Array<float, 4> floats;
    Random rng;
    int counter = 7;
    int hash = 1;
    Mode current = Mode::Add;
    bool flip = false;
    double phase = 0.0;

    void prepare() {
        rng.seed(1234);
        fillRamp(taps, -1.0);
        for (auto& f : floats) f = kHalf;
    }

    void update() {
        current = mode.index() == 0 ? Mode::Add : Mode::Multiply;
        if (steps.changed()) counter = steps.index() * 3;
    }

    double step(double x, int i) {
        Accumulator& acc = stats[i & 1];
        acc.add(x);
        double y = x;
        switch (current) {
            case Mode::Add:
                y = x + acc.mean() * amount;
                break;
            case Mode::Multiply:
                y = x * (1.0 + acc.mean());
                break;
        }
        // float arithmetic stays float; mixed promotes to double.
        float f = floats[i % 4] * 1.5f - 0.25f;
        floats[i % 4] = f > 4.0f ? 0.125f : f;
        y += f;
        y += static_cast<double>(floats[(i + 1) % 4]) * 0.5;
        // Integers: wrap, shifts by constants, remainders by constants.
        hash = scramble(hash, i);
        const int bits = (hash >> 4) & 255;
        y += (bits % 7 - 3) * 0.01;
        // Out of range on purpose: a spare element reads 0.
        y += taps[i % 11] * 0.001;
        // Built-ins: math, conversions, the random generator.
        y = clamp(y, -4.0, 4.0) * 0.5 + 0.1 * sin(phase) + 0.01 * rng.bipolar();
        phase += twoPi * 0.01;
        if (phase >= twoPi) phase -= twoPi;
        int k = toInt(y * 100.0);
        k = max(min(k, 300), -300);
        y += k * 1e-4;
        y += idiv(k, 7) * 1e-5 + imod(k, 5) * 1e-6 + wrap(k, 13) * 1e-7;
        flip = !flip;
        y = flip ? y : -y * 0.5;
        y += average(taps) * 1e-3;  // which keeps rising: the output is bounded below
        return on ? clamp(y, -1.0, 1.0) : x;
    }

    void process(Block& io) {
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                double x = io[c][i];
                io[c][i] = step(x, i + c * 3);
            }
            --counter;
            if (counter <= 0) {
                counter = steps.index() * 3;
                taps[counter % kTaps] += 0.5;
            }
        }
        // Writing past the last channel lands in the spare sample.
        io[io.channels][0] = 1.0;
    }
};
