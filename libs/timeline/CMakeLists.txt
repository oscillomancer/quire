# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(timeline SOURCES src/timeline.cpp DEPS supple::core-base)
supple_add_tests(timeline SOURCES tests/test_timeline.cpp)
