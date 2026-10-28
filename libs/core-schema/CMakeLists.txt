# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
supple_add_library(core-schema SOURCES src/schema.cpp src/document.cpp DEPS supple::core-json)
supple_add_tests(core-schema SOURCES tests/test_schema.cpp)
