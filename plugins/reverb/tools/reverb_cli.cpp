// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstdio>
#include <string>
#include "reverb/processor.h"
#include "wav.h"
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "reverb-cli input.wav output.wav [key value]...\n");
        return 2;
    }
    wavio::Audio audio;
    std::string error;
    if (!wavio::read(argv[1], audio, error) || audio.channels < 1 || audio.channels > 2) return 1;
    supple::reverb::Processor p;
    if (!p.prepare(audio.sampleRate, 512, audio.channels)) return 1;
    for (int i = 3; i + 1 < argc; i += 2) {
        const auto* spec = supple::params::findByKey(supple::reverb::table(), argv[i]);
        if (!spec) return 2;
        auto v = supple::params::parse(*spec, argv[i + 1]);
        if (!v) return 2;
        p.setParam(spec->id, *v);
    }
    for (size_t off = 0; off < audio.frames(); off += 512) {
        const double* in[2]{};
        double* out[2]{};
        for (int c = 0; c < audio.channels; ++c) {
            in[c] = audio.ch[static_cast<size_t>(c)].data() + off;
            out[c] = audio.ch[static_cast<size_t>(c)].data() + off;
        }
        p.process(in, out, static_cast<uint32_t>(std::min<size_t>(512, audio.frames() - off)));
    }
    return wavio::write(argv[2], audio, 32, error) ? 0 : 1;
}
