// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <filesystem>
#include <string_view>
#include "supple/base/result.h"
namespace supple::agent {
Status writePrivateConfig(const std::filesystem::path& file, std::string_view text);
}
