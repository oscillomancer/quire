# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(model
  SOURCES src/session.cpp src/schemas.cpp src/strips.cpp src/migration.cpp src/versions.cpp
  DEPS supple::core-base supple::core-json supple::core-schema supple::midi supple::timeline)
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(model SOURCES tests/test_model.cpp tests/test_versions.cpp)
endif()
