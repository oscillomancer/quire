// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A NanoVG context with no GPU behind it: every draw call is accepted and
// discarded. Enough to run an editor's real paint(), font loading and repaint
// logic in a test with no display.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include <nanovg.h>

namespace supple::test {

inline NVGparams headlessParams() {
    NVGparams p{};
    p.renderCreate = [](void*) { return 1; };
    p.renderCreateTexture = [](void*, int, int, int, int, const unsigned char*) { return 1; };
    p.renderDeleteTexture = [](void*, int) { return 1; };
    p.renderUpdateTexture = [](void*, int, int, int, int, int, const unsigned char*) { return 1; };
    p.renderGetTextureSize = [](void*, int, int* w, int* h) {
        *w = *h = 512;
        return 1;
    };
    p.renderViewport = [](void*, float, float, float) {};
    p.renderCancel = [](void*) {};
    p.renderFlush = [](void*) {};
    p.renderFill = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, const float*, const NVGpath*,
                      int) {};
    p.renderStroke = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float, const NVGpath*,
                        int) {};
    p.renderTriangles = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, const NVGvertex*, int,
                           float) {};
    p.renderDelete = [](void*) {};
    return p;
}

// An optional hash records stroke geometry for editor drawing regressions.
inline NVGcontext* createHeadlessCanvas(uint64_t* strokeHash = nullptr) {
    NVGparams p = headlessParams();
    p.userPtr = strokeHash;
    p.renderStroke = [](void* user, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float,
                        const NVGpath* paths, int count) {
        if (!user) return;
        auto& hash = *static_cast<uint64_t*>(user);
        for (int p = 0; p < count; ++p) for (int v = 0; v < paths[p].nstroke; ++v) {
            for (float position : {paths[p].stroke[v].x, paths[p].stroke[v].y}) {
                uint32_t bits = 0;
                std::memcpy(&bits, &position, sizeof(bits));
                hash = (hash ^ bits) * UINT64_C(1099511628211);
            }
        }
    };
    return nvgCreateInternal(&p);
}

// Records, for each path of each fill, which way NanoVG winds it: +1 solid,
// -1 a hole (nvgPathWinding), as its nonzero fill will count it.
inline NVGcontext* createFillRecorder(std::vector<std::vector<int>>* windings) {
    NVGparams p = headlessParams();
    p.userPtr = windings;
    p.renderFill = [](void* user, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, const float*,
                      const NVGpath* paths, int count) {
        auto& fills = *static_cast<std::vector<std::vector<int>>*>(user);
        fills.emplace_back();
        for (int p = 0; p < count; ++p) {
            float area = 0.0f;
            const NVGvertex* v = paths[p].fill;
            for (int i = 2; i < paths[p].nfill; ++i)
                area += (v[i - 1].x - v[0].x) * (v[i].y - v[0].y) - (v[i].x - v[0].x) * (v[i - 1].y - v[0].y);
            fills.back().push_back(area < 0.0f ? 1 : -1);  // NanoVG's fill vertices run against its winding
        }
    };
    return nvgCreateInternal(&p);
}

// Records the clip each fill and stroke is drawn under, as {left, top,
// right, bottom}; an unclipped one as all zeros. Drawn untransformed.
using Clip = std::array<float, 4>;
inline NVGcontext* createClipRecorder(std::vector<Clip>* clips) {
    NVGparams p = headlessParams();
    p.userPtr = clips;
    static constexpr auto record = [](void* user, const NVGscissor* s) {
        Clip clip{};
        if (s->extent[0] >= 0.0f)
            clip = {s->xform[4] - s->extent[0], s->xform[5] - s->extent[1], s->xform[4] + s->extent[0],
                    s->xform[5] + s->extent[1]};
        static_cast<std::vector<Clip>*>(user)->push_back(clip);
    };
    p.renderFill = [](void* user, NVGpaint*, NVGcompositeOperationState, NVGscissor* s, float, const float*,
                      const NVGpath*, int) { record(user, s); };
    p.renderStroke = [](void* user, NVGpaint*, NVGcompositeOperationState, NVGscissor* s, float, float,
                        const NVGpath*, int) { record(user, s); };
    return nvgCreateInternal(&p);
}

}  // namespace supple::test
