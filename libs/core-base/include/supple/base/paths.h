// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>

namespace supple::paths {
// Platform directory roots, without the product slug. No directory creation.
// Empty config/data/cache means no user directory is available. Runtime always
// has a temporary-directory fallback. data/cache allowTemporary preserve the
// CLI fallback when no user directory exists. Call only off audio, before workers start.
std::filesystem::path home();
std::filesystem::path config();
std::filesystem::path data(bool allowTemporary = false);
std::filesystem::path cache(bool allowTemporary = false);
std::filesystem::path runtime();
}  // namespace supple::paths
