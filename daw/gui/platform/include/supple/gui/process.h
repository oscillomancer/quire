// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Control/UI thread diagnostics, never called from an audio path.
#pragma once
namespace supple::gui {
unsigned long processId();
double millisecondsSinceProcessStart();  // -1 when unavailable
double processCpuMs();
double threadCpuMs();
double wallMs();
}  // namespace supple::gui
