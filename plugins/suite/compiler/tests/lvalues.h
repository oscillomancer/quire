// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

struct Lvalues : Effect {
    static constexpr Info info = {.name = "Lvalues", .id = "com.example.lvalues"};

    void process(Block& io) {
        Array<int, 4> a;
        int i = 1;
        a[++i] = i;
        io[0][0] = a[2];
        i = 0;
        (a[++i] = 1) = 2;
        io[0][1] = i;
        io[0][2] = a[1];
        i = 0;
        (++a[++i]) = 4;
        io[0][3] = i;
        io[0][4] = a[1];
        double x = 1.0;
        double y = 2.0;
        (io.frames > 0 ? x : y) = 3.0;
        io[0][5] = x;
        int n = 7;
        int& r = (n = 8);
        r = 9;
        io[0][6] = n;
        const double& d = n;
        n = 10;
        io[0][7] = d;
        i = 0;
        a[++i] += i;
        io[0][8] = a[1];
    }
};
