// SPDX-License-Identifier: MIT
#include "analyzer/processor.h"
int main() {
    supple::analyzer::Processor processor;
    processor.prepare(48000, 256, 1);
    float samples[]{.25f, -.5f, .125f};
    float* io[]{samples};
    processor.process(io, 3);
    return samples[0] == .25f && samples[1] == -.5f && samples[2] == .125f &&
                   processor.latency() == 0
               ? 0
               : 1;
}
