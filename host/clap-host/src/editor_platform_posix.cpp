// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "editor_platform.h"
namespace supple::host {
Status initializeEditorPlatform() { return {}; }
int waitEditorEvents(std::span<WaitItem> items, int timeoutMs) { return waitFor(items, timeoutMs); }
}  // namespace supple::host
