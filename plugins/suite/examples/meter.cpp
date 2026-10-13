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
