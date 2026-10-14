// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "supple/host/clap_host.h"

namespace supple::host {
// Internal, used only for trusted modules and the bridge's --scan mode.
Result<std::vector<PluginInfo>> scanModuleInProcess(const std::filesystem::path& module);
json::Value scanReply(const Result<std::vector<PluginInfo>>& result);
Result<std::vector<PluginInfo>> parseScanReply(const json::Value& reply,
                                               const std::filesystem::path& module);
}  // namespace supple::host
