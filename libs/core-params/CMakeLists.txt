# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
supple_add_library(core-params
  SOURCES src/params.cpp
  DEPS supple::core-base supple::core-json
  PRIVATE_DEPS supple::clap)

supple_add_tests(core-params SOURCES tests/test_params.cpp DEPS supple::clap LABELS rt)
