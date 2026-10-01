// SPDX-License-Identifier: MIT
#pragma once
#include <memory>
#include "supple/console9/console9.h"

namespace supple::console9::test {
class Reference {
  public:
    virtual ~Reference() = default;
    // Prime endpoints without running audio or changing the dither streams.
    virtual void set(float fader, float pan, Seeds seeds) = 0;
    virtual void process(float** in, float** out, int32_t n) = 0;
    virtual void process(double** in, double** out, int32_t n) = 0;
};
std::unique_ptr<Reference> channelReference();
std::unique_ptr<Reference> bussReference();
}  // namespace supple::console9::test
