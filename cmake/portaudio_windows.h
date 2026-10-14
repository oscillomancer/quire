// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The stable vendor sources assume full Windows/COM declarations, while the
// DAW uses WIN32_LEAN_AND_MEAN. Supply their requirements without editing them.
#include <windows.h>

#include <winternl.h>

// ASIO C++ needs COM's interface macro; the C frontend uses that identifier.
// WASAPI C includes its own COM declarations after this prelude.
#if defined(__cplusplus)
#include <objbase.h>
#endif
