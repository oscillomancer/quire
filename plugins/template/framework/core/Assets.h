// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Files the product compiles in: everything listed in PRODUCT_ASSETS in
// product.cmake (a logo, icons, an image skin, a font, an impulse
// response), found by file name. A plugin has nowhere reliable to load a
// file from -- bundle layouts differ per format and host -- so they travel
// inside the binary, as the framework's fonts do.
//
//   PRODUCT_ASSETS product/assets/logo.svg product/assets/Orbitron.ttf
//
// The editor draws SVGs and images by name (EditorBase::drawSvg, drawImage)
// and registers fonts by file stem ("Orbitron"); anything else is bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" {
struct supple_asset {
    const char* name;
    const unsigned char* data;
    size_t size;
};
// Generated from PRODUCT_ASSETS (Framework.cmake).
extern const supple_asset supple_assets[];
extern const size_t supple_asset_count;
}

namespace supple::assets {

struct Asset {
    std::string_view name;  // the file name, "logo.svg"
    const uint8_t* data = nullptr;
    size_t size = 0;
    explicit operator bool() const { return data != nullptr; }
    std::string_view text() const { return {reinterpret_cast<const char*>(data), size}; }
};

inline size_t count() { return supple_asset_count; }
inline Asset at(size_t i) {
    return i < supple_asset_count ? Asset{supple_assets[i].name, supple_assets[i].data, supple_assets[i].size} : Asset{};
}
// By file name; an empty Asset when there is none.
inline Asset find(std::string_view name) {
    for (size_t i = 0; i < supple_asset_count; ++i)
        if (name == supple_assets[i].name) return at(i);
    return {};
}
// The name's extension, lower case, matches ".svg" and the like.
inline bool hasExtension(std::string_view name, std::string_view extension) {
    if (name.size() < extension.size()) return false;
    const std::string_view tail = name.substr(name.size() - extension.size());
    for (size_t i = 0; i < tail.size(); ++i) {
        const char c = tail[i] >= 'A' && tail[i] <= 'Z' ? static_cast<char>(tail[i] - 'A' + 'a') : tail[i];
        if (c != extension[i]) return false;
    }
    return true;
}

}  // namespace supple::assets
