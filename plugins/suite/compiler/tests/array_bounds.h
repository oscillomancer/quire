// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

struct ArrayBounds : Effect {
    static constexpr Info info = {.name = "Array bounds", .id = "com.example.array-bounds"};
    enum class Mode { Off, On };
    struct Cell {
        double value = 3.5;
        Mode mode = Mode::On;
        Array<int, 2> numbers = {4, 5};
    };
    struct Resource {
        DelayLine delay;
        double value = 2.0;
    };
    Array<Cell, 1> cells;
    Array<Array<Cell, 1>, 1> nested;
    Array<Resource, 1> resources;

    void process(Block& io) {
        cells[0].value = 9.0;
        cells[2].value = 7.0;
        io[0][0] = cells[-1].value;
        cells[2].mode = Mode::Off;
        io[0][1] = static_cast<int>(cells[2].mode);
        cells[2].numbers[0] = 99;
        io[0][2] = cells[-1].numbers[0];
        nested[3][0].value = 8.0;
        io[0][3] = nested[-1][0].value;
        resources[3].value = 7.0;
        io[0][4] = resources[-1].value;
        io[0][5] = cells[0].value;
        // References alias the spare until the next invalid access resets it.
        Cell& held = cells[3];
        held.value = 11.0;
        io[0][6] = held.value;
        io[0][7] = cells[4].value;
        io[0][8] = held.value;
    }
};
