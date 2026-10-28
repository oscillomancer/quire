// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <memory>

#include "supple/script_native.h"

namespace supple::script::test_marram {
#include "../../marram/script/marram.cpp"
}

TEST_CASE("Marram free LFOs do not depend on the block partition") {
    namespace S = supple::script;
    namespace D = S::detail;
    using T = S::test_marram::Marram;
    const D::NativeCode<T> code;
    D::Instance instance;
    instance.maxBlockSize = 1024;
    D::CurrentScope scope(&instance);
    auto make = [&] {
        auto p = std::make_unique<T>();
        D::Instance::bind(*p, &instance);
        for (const auto& spec : code.params()) {
            auto* param = reinterpret_cast<S::Param*>(reinterpret_cast<unsigned char*>(p.get()) +
                                                      spec.offset);
            D::ParamBinding::setValue(
                *param, spec.unit == D::ParamModel::Unit::Percent ? spec.def * 0.01 : spec.def,
                true);
        }
        D::ParamBinding::setValue(p->l1Mode, 1, true);
        p->prepare();
        p->update();
        return p;
    };
    auto a = make(), b = make();
    std::array<double, 1024> left{}, right{};
    double* data[] = {left.data(), right.data()};
    double spare = 0.0;
    S::Block io;
    io.channels = 2;
    io.data_ = data;
    io.spare_ = &spare;
    const auto process = [&](T& p, int partition)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
                             [[clang::nonblocking]]
#endif
    {
        for (int at = 0; at < 1024; at += io.frames) {
            io.frames = std::min(partition, 1024 - at);
            p.process(io);
        }
    };
    for (double hz : {0.12, 3.5, 0.3, 5.5}) {
        D::ParamBinding::setValue(a->l1Rate, hz, true);
        D::ParamBinding::setValue(b->l1Rate, hz, true);
        for (int step = 0; step < 4; ++step) {
            process(*a, 1);
            process(*b, 511);
            const auto value = [](T& p) {
                return p.lfo(p.voice[0], 0, 0, 0, p.l1Shape, p.l1Sync, p.l1Rate, p.l1Division,
                             p.l1Mode, p.l1Phase, p.l1Delay);
            };
            CHECK(value(*a) == value(*b));
        }
    }
}
