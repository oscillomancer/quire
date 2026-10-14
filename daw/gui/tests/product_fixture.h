// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// One fixture identity, derived from the generated product configuration.
#pragma once
#include <string>
#include "supple/product.h"
namespace supple::gui::test {
inline std::string slug() { return std::string(product::kSlug) + "-test"; }
inline std::string name() { return std::string(product::kName) + " test"; }
}  // namespace supple::gui::test
