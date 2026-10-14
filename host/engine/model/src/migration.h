// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/model/schemas.h"

namespace supple::model {
Result<json::Value> migrateProject1(json::Value doc, std::vector<schema::Warning>* warnings);
}
