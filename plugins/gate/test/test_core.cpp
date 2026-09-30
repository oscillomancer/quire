// SPDX-License-Identifier: MIT
#include <cmath>
#include "gate/processor.h"
int main() {
    supple::gate::Processor p;
    p.prepare(48000, 64);
    float l[64]{}, r[64]{};
    float* io[]{l, r};
    p.process(io, 64);
    for (float v : l)
        if (v != 0) return 1;
    return 0;
}
