// Implicit console stages. All state is owned by the source strip's executor.
#pragma once
#include <algorithm>
#include <cmath>
#include "supple/console9/console9.h"

namespace supple::engine::detail {
// Gain and native pan ramps match the library's advance-before-sample rule.
// Evaluating controls separately lets mute ramp even under continuous automation.
struct ControlRamp {
    double value = 0, target = 0, step = 0;
    std::uint32_t left = 0;
    void set(double v, std::uint32_t n) SUPPLE_RT {
        if (v == target && (n || !left)) return;
        target = v;
        left = n;
        if (n)
            step = (v - value) / n;
        else
            value = v;
    }
    double next() SUPPLE_RT {
        if (left && --left)
            value += step;
        else if (!left)
            value = target;
        return value;
    }
};
struct FaderControl {
    ControlRamp gain, pan, gate;
    bool valid = false;
    void set(double g, double p, bool muted, bool automated, std::uint32_t frames) SUPPLE_RT {
        if (!valid) {
            gain.value = gain.target = g;
            pan.value = pan.target = p;
            gate.value = gate.target = muted ? 0 : 1;
            valid = true;
        }
        gain.set(g, automated ? 0 : frames);
        pan.set(p, automated ? 0 : frames);
        gate.set(muted ? 0 : 1, frames);
    }
    void reset() SUPPLE_RT { *this = {}; }
};

template <class Stage>
struct ConsoleStage {
    Stage current, previous;
    console9::Seeds seeds;
    bool linear = false;
    std::uint32_t left = 0, span = 1;
    double lastGain = 1, lastPan = .5, fadeFrom = 0;
    void init(console9::Seeds s, bool off) {
        seeds = s;
        linear = off;
        current.set(1, .5);
        current.setLinear(off);
        current.reset(s);
    }
    void reset() SUPPLE_RT {
        left = 0;
        current.reset(seeds);
        previous.reset(seeds);
    }
    void carry(const ConsoleStage& old, std::uint32_t frames) SUPPLE_RT {
        current = old.current;
        lastGain = old.lastGain;
        lastPan = old.lastPan;
        if (linear != old.linear) {
            previous = old.current;
            fadeFrom = 0;
            if (old.left) {
                // Reversing a two-mode fade starts from the mix actually heard.
                current = old.previous;
                fadeFrom = (1 - old.fadeFrom) * old.left / old.span;
            }
            left = span = frames;
        } else {
            previous = old.previous;
            left = old.left;
            span = old.span;
            fadeFrom = old.fadeFrom;
        }
        current.setLinear(linear);
    }
    void sample(float l, float r, float& ol, float& or_, double gain = 1,
                double pan = .5) SUPPLE_RT {
        const float* in[] = {&l, &r};
        float* out[] = {&ol, &or_};
        // Sample-accurate controls, including the pre-encode mute ramp.
        const bool changed = gain != lastGain || pan != lastPan;
        if (changed) current.set(gain, pan);
        current.process(in, out, 1);
        if (left) {
            float pl, pr;
            float* po[] = {&pl, &pr};
            if (changed) previous.set(gain, pan);
            previous.process(in, po, 1);
            const float t = static_cast<float>(fadeFrom + (1 - fadeFrom) * (span - --left) / span);
            ol = pl + (ol - pl) * t;
            or_ = pr + (or_ - pr) * t;
        }
        lastGain = gain;
        lastPan = pan;
    }
};
}  // namespace supple::engine::detail
