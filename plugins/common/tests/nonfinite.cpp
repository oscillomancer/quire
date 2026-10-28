// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "supple/script_native.h"

namespace supple::script::test_bracken {
#include "../../bracken/script/bracken.cpp"
}
namespace supple::script::test_umber {
#include "../../umber/script/umber.cpp"
}
namespace supple::script::test_ghyll {
#include "../../ghyll/script/ghyll.cpp"
}
namespace supple::script::test_pollia {
#include "../../pollia/script/pollia.cpp"
}
namespace supple::script::test_withy {
#include "../../withy/script/withy.cpp"
}

namespace {
namespace S = supple::script;
namespace D = S::detail;
constexpr int kFrames = 4096;
using Audio = std::array<std::array<double, kFrames>, 2>;

template <class T>
void process(T& plugin, Audio& audio, int channels)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
    [[clang::nonblocking]]
#endif
{
    double* data[] = {audio[0].data(), audio[1].data()};
    double spare = 0.0;
    S::Block block;
    block.channels = channels;
    block.frames = kFrames;
    block.data_ = data;
    block.spare_ = &spare;
    plugin.process(block);
}

template <class T>
void checkNonfinite() {
    const D::NativeCode<T> code;
    for (int channels : {1, 2}) {
        CAPTURE(channels);
        D::Instance instance;
        instance.channels = channels;
        instance.maxBlockSize = kFrames;
        D::CurrentScope scope(&instance);
        auto make = [&] {
            auto plugin = std::make_unique<T>();
            D::Instance::bind(*plugin, &instance);
            for (const auto& p : code.params()) {
                auto* param = reinterpret_cast<S::Param*>(
                    reinterpret_cast<unsigned char*>(plugin.get()) + p.offset);
                const double value = p.unit == D::ParamModel::Unit::Percent ? p.def * 0.01 : p.def;
                D::ParamBinding::setValue(*param, value, true);
            }
            plugin->prepare();
            plugin->update();
            return plugin;
        };
        auto tested = make(), reference = make();
        Audio actual{}, expected{};
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < kFrames; ++i)
                actual[c][i] = expected[c][i] = 0.1 * std::sin(i * 0.07);
        for (int c = 0; c < channels; ++c) {
            actual[c][0] = std::numeric_limits<double>::quiet_NaN();
            actual[c][1] = std::numeric_limits<double>::infinity();
            actual[c][2] = -std::numeric_limits<double>::infinity();
            expected[c][0] = expected[c][1] = expected[c][2] = 0.0;
        }
        process(*tested, actual, channels);
        process(*reference, expected, channels);
        CHECK(std::memcmp(actual.data(), expected.data(), sizeof(Audio)) == 0);
        // The contaminated samples must not remain in delay/filter history.
        actual = {};
        expected = {};
        process(*tested, actual, channels);
        process(*reference, expected, channels);
        CHECK(std::memcmp(actual.data(), expected.data(), sizeof(Audio)) == 0);
        bool finite = true;
        for (const auto& channel : actual)
            for (double sample : channel) finite &= std::isfinite(sample);
        CHECK(finite);
    }
}
}  // namespace

TEST_CASE("Ghyll keeps non-finite input out of recursive state") {
    checkNonfinite<S::test_ghyll::Ghyll>();
}
TEST_CASE("Pollia keeps non-finite input out of recursive state") {
    checkNonfinite<S::test_pollia::Pollia>();
}
TEST_CASE("Withy keeps non-finite input out of recursive state") {
    checkNonfinite<S::test_withy::Withy>();
}
TEST_CASE("Bracken keeps non-finite input out of recursive state") {
    checkNonfinite<S::test_bracken::Bracken>();
}
TEST_CASE("Umber keeps non-finite input out of recursive state") {
    checkNonfinite<S::test_umber::Umber>();
}
