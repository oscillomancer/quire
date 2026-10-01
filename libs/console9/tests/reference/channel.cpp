// SPDX-License-Identifier: MIT
#include <set>
#include <string>
#include "audioeffectx.h"
#include "reference.h"
// Access control only; all standard headers are included before the macro.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "Console9Channel/Console9Channel.h"
#undef private
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace supple::console9::test {
namespace {
class Adapter final : public Reference {
    Console9Channel effect_{nullptr};

  public:
    void set(float fader, float pan, Seeds seeds) override {
        effect_.setParameter(kParamA, pan);
        effect_.setParameter(kParamB, fader);
        effect_.panA = effect_.panB = double(pan) * 1.57079633;
        effect_.inTrimA = effect_.inTrimB = double(fader) * 2.0;
        effect_.fpdL = seeds.left;
        effect_.fpdR = seeds.right;
    }
    void process(float** in, float** out, int32_t n) override {
        effect_.processReplacing(in, out, n);
    }
    void process(double** in, double** out, int32_t n) override {
        effect_.processDoubleReplacing(in, out, n);
    }
};
}  // namespace
std::unique_ptr<Reference> channelReference() { return std::make_unique<Adapter>(); }
}  // namespace supple::console9::test
