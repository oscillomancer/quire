// SPDX-License-Identifier: MIT
#include <cmath>
#include "reverb/processor.h"
int main() {
    supple::reverb::Processor p;
    if (!p.prepare(48000, 64)) return 1;
    float l[64]{}, r[64]{};
    const float* in[]{l, r};
    float* out[]{l, r};
    p.process(in, out, 64);
    for (float x : l)
        if (!std::isfinite(x) || x != 0) return 1;
    return 0;
}
