// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Proves the rtsan preset really enforces real-time safety: this program
// allocates inside a SUPPLE_RT function and must be killed by
// RealtimeSanitizer. Registered as WILL_FAIL in the rtsan preset only.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <supple/base/rt.h>

static volatile int sink;

static int allocateInRealtime() SUPPLE_RT {
    auto p = std::make_unique<int[]>(64);  // must trip rtsan
    p[3] = 7;
    return p[3];
}

int main() {
    sink = allocateInRealtime();
    std::puts("rtsan did not fire");
    return 0;
}
