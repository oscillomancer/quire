// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// One band of an equaliser, shaped by hand in an editor of its own: press on
// the graph and drag to move the bell (across for its frequency, up or down
// to boost or cut), turn the wheel over it to make it narrower or wider, and
// double-click to start again. The curve is the filter's own response.
struct Bell : Effect {
    static constexpr Info info = {.name = "Bell", .id = "com.example.bell", .vendor = "Example"};

    Param freq = frequency("Frequency", 20, 20000, 1000);
    Param gain = decibels("Gain", -18, 18, 0).help("Boost above zero, cut below.");
    Param width = octaves("Width", 0.25, 4, 1).help("The bell's width at half its height.");

    Array<Biquad, 2> bell;

    // The graph: 20 Hz to 20 kHz across, -18 to +18 dB up.
    static constexpr double gx = 40;
    static constexpr double gy = 24;
    static constexpr double gw = 416;
    static constexpr double gh = 216;

    // The editor's own: a filter to draw the response with, and the node's
    // state (held by a press; lit while the pointer is on it).
    Biquad shape;
    bool held = false;
    bool lit = false;

    void update() {
        for (int c = 0; c < 2; ++c) bell[c].peak(freq, q(width), gain);
    }

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] = bell[c].process(io[c][i]);
    }

    // A bell's Q for its width in octaves.
    double q(double octaves) const {
        const double b = pow(2.0, octaves);
        return sqrt(b) / (b - 1.0);
    }
    double nodeX() const { return hzToX(freq, 20, 20000, gx, gw); }
    double nodeY() const { return dbToY(gain, -18, 18, gy, gh); }

    void editor(Editor& e) { e.size(480, 300); }

    void draw(Canvas& g) {
        g.well(gx, gy, gw, gh);
        g.frequencyGrid(gx, gy, gw, gh, 20, 20000);
        g.levelGrid(gx, gy, gw, gh, -18, 18, 6);

        shape.peak(freq, q(width), gain);
        g.save();
        g.clip(gx, gy, gw, gh);
        g.strokeStyle(Theme::accent);
        g.lineWidth(2);
        g.beginPath();
        for (int k = 0; k <= 216; ++k) {
            const double x = gx + gw * k / 216.0;
            const double y = dbToY(shape.responseDb(xToHz(x, 20, 20000, gx, gw)), -18, 18, gy, gh);
            if (k == 0)
                g.moveTo(x, y);
            else
                g.lineTo(x, y);
        }
        g.stroke();
        g.restore();

        g.fillStyle(held ? Theme::accent : Theme::raised);
        g.strokeStyle(held || lit ? Theme::text : Theme::accent);
        g.lineWidth(2);
        g.beginPath();
        g.circle(nodeX(), nodeY(), held || lit ? 8 : 6);
        g.fill();
        g.stroke();

        g.font(13);
        g.fillStyle(Theme::textMuted);
        g.fillText(freq.text(), gx, 284);
        g.fillText(gain.text(), gx + 144, 284);
        g.fillText(width.text(), gx + 288, 284);
        g.serifFont(15);
        g.fillStyle(Theme::text);
        g.textAlign(TextAlign::Right);
        g.fillText("Bell", gx + gw, 284);
        g.textAlign(TextAlign::Left);
    }

    void pointer(const Pointer& p) {
        if (p.action() == PointerAction::Press && p.over(gx, gy, gw, gh)) {
            if (p.doubleClick()) {
                freq.reset();
                gain.reset();
                width.reset();
            } else {
                held = true;
                place(p.x(), p.y());
            }
        } else if (p.action() == PointerAction::Drag && held) {
            place(p.x(), p.y());
        } else if (p.action() == PointerAction::Release) {
            held = false;
        } else if (p.action() == PointerAction::Wheel && p.over(gx, gy, gw, gh)) {
            width.edit(width - 0.25 * p.wheel());  // up: narrower
        }
        lit = p.action() != PointerAction::Leave && p.within(nodeX(), nodeY(), 12);
    }

    // The bell to where the pointer is, kept on the graph.
    void place(double x, double y) {
        freq.edit(xToHz(clamp(x, gx, gx + gw), 20, 20000, gx, gw));
        gain.edit(yToDb(clamp(y, gy, gy + gh), -18, 18, gy, gh));
    }
};
