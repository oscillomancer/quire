// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The stable vendor sources assume full Windows/COM declarations, while the
// DAW uses WIN32_LEAN_AND_MEAN. Supply their requirements without editing them.
#include <windows.h>

#include <objbase.h>
#include <winternl.h>
