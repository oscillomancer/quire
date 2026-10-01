// Minimal PNG writer for snapshots and golden images: RGBA8, deflate
// "stored" blocks (no compression library needed; files are larger, which is
// fine for test artifacts).
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "supple/base/result.h"
#include "supple/gui/window.h"

namespace supple::gui {

std::vector<std::uint8_t> encodePng(const Image& image);
Status writePng(const std::filesystem::path& path, const Image& image);

}  // namespace supple::gui
