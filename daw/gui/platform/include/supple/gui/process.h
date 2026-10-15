// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Control/UI thread diagnostics, never called from an audio path.
#pragma once
#include <ctime>
namespace supple::gui {
unsigned long processId();
std::tm localTime(std::time_t time);
double millisecondsSinceProcessStart();  // -1 when unavailable
double processCpuMs();
double threadCpuMs();
double wallMs();
}  // namespace supple::gui
