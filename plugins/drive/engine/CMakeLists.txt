supple_add_library(drive-engine SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
supple_add_tests(drive-engine SOURCES tests/test_drive.cpp DEPS supple::core-analysis LABELS rt)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(drive-engine.tests PRIVATE
    DRIVE_GUIDE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../../skill/guides/drive.md")
  # TSan instruments the long FIR sweeps; retain the full numerical coverage.
  if(SUPPLE_SANITIZE STREQUAL "thread")
    set_tests_properties(drive-engine.tests PROPERTIES TIMEOUT 2400)
  endif()
endif()
