# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# CMA-ES (docs/specs/optimizer.md §1): deterministic derivative-free minimization.
supple_add_library(optimize SOURCES src/cmaes.cpp DEPS supple::core-base)
supple_add_tests(optimize SOURCES tests/test_cmaes.cpp)
