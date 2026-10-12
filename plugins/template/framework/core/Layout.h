// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Channel layouts a bus can take, named in product.cmake's PRODUCT_CHANNELS:
//
//   1 2            mono, stereo
//   5.1 7.1 7.1.4  surround, channels in the order below (the WAVE and VST3
//                  order: each speaker's CLAP position, ascending)
//   ambi1..ambi3   ambisonics of order 1 to 3, ACN channel order with SN3D
//                  normalisation (AmbiX)
//   3..64          that many channels with no speaker meaning
//
// A processor that cares which channel is which (an LFE to leave alone, a
// height layer) reads ProcessSetup::layout; the rest can treat channels alike.
#pragma once

#include <cstdint>

namespace supple {

enum class Layout : uint8_t {
    Generic,      // N channels, no speaker positions
    Mono,
    Stereo,
    Surround51,   // L R C LFE Ls Rs
    Surround71,   // L R C LFE Ls Rs Lss Rss (rear, then side surrounds)
    Surround714,  // 7.1 plus Ltf Rtf Ltr Rtr
    Ambisonic1,   // 4 channels
    Ambisonic2,   // 9
    Ambisonic3,   // 16
};

// Speaker positions, numbered as CLAP's surround extension numbers them
// (CLAP_SURROUND_FL = 0 ...), which is also the bit order of a VST3 speaker
// arrangement and of a WAVE channel mask.
namespace speaker {
inline constexpr uint8_t kFL = 0, kFR = 1, kFC = 2, kLFE = 3, kBL = 4, kBR = 5, kSL = 9, kSR = 10, kTFL = 12,
                         kTFR = 14, kTBL = 15, kTBR = 17;
}

constexpr bool isSurround(Layout l) {
    return l == Layout::Surround51 || l == Layout::Surround71 || l == Layout::Surround714;
}
constexpr bool isAmbisonic(Layout l) {
    return l == Layout::Ambisonic1 || l == Layout::Ambisonic2 || l == Layout::Ambisonic3;
}

namespace detail {
using namespace speaker;
inline constexpr uint8_t k51[] = {kFL, kFR, kFC, kLFE, kBL, kBR};
inline constexpr uint8_t k71[] = {kFL, kFR, kFC, kLFE, kBL, kBR, kSL, kSR};
inline constexpr uint8_t k714[] = {kFL, kFR, kFC, kLFE, kBL, kBR, kSL, kSR, kTFL, kTFR, kTBL, kTBR};
}  // namespace detail

// The speaker at each channel of a surround layout, in channel order; the
// count is the layout's channel count. Null for every other layout.
constexpr const uint8_t* speakers(Layout l) {
    switch (l) {
        case Layout::Surround51: return detail::k51;
        case Layout::Surround71: return detail::k71;
        case Layout::Surround714: return detail::k714;
        default: return nullptr;
    }
}

// The bit of every speaker in a surround layout; 0 for any other.
constexpr uint64_t speakerMask(Layout l, int channels) {
    const uint8_t* s = speakers(l);
    uint64_t mask = 0;
    for (int c = 0; s && c < channels; ++c) mask |= uint64_t{1} << s[c];
    return mask;
}

// The channel carrying low-frequency effects, or -1.
constexpr int lfeChannel(Layout l) { return isSurround(l) ? 3 : -1; }

}  // namespace supple
