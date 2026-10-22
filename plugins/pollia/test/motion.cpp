// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <cstdio>

#include "supple/script_runtime.h"

#include "../script/pollia.cpp"

int main() {
    using Binding = supple::script::detail::ParamBinding;
    for (int mode = 0; mode < 3; ++mode) {
        for (double beats : {-1.25, -0.25, 0.0, 0.25, 4.125}) {
            Pollia p;
            p.prepare();
            Binding::setValue(p.sync, 6, true);  // quarter-note sync: one beat per cycle
            Binding::setValue(p.motion, mode, true);
            p.update();

            double sample = 0.0, spare = 0.0;
            double* samples[] = {&sample};
            Block block;
            block.channels = 1;
            block.frames = 1;
            block.data_ = samples;
            block.spare_ = &spare;
            block.transport.playing = true;
            block.transport.tempo = 120.0;
            block.transport.beats = beats;
            p.process(block);

            const int cycle = floorToInt(beats);
            const double phase = beats - cycle;
            const double turn = beats * (mode == 1 ? -1.0 : 1.0);
            if (p.lfoCycle != cycle || p.lfoPhase != phase || p.turn != turn) {
                std::fprintf(stderr,
                             "mode %d at %.3f beats: cycle %d, phase %.3f, turn %.3f; "
                             "expected %d, %.3f, %.3f\n",
                             mode, beats, p.lfoCycle, p.lfoPhase, p.turn, cycle, phase, turn);
                return 1;
            }
        }
    }
    return 0;
}
