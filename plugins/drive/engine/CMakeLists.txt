include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/Identity.cmake")
supple_add_library(drive-engine SOURCES src/params.cpp src/processor.cpp
  DEPS supple::core-base supple::core-params supple::core-patch supple::core-dsp)
target_compile_definitions(drive-engine PUBLIC SUPPLE_DRIVE_DISPLAY_NAME="${DRIVE_DISPLAY_NAME}")
supple_add_tests(drive-engine SOURCES tests/test_drive.cpp DEPS supple::core-analysis LABELS rt)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(drive-engine.tests PRIVATE
    DRIVE_GUIDE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../../skill/guides/drive.md")
  # Opt-in, unregistered measurement runner preserves the documented dense sweeps.
  add_executable(drive-engine.measure EXCLUDE_FROM_ALL tests/test_drive.cpp)
  target_link_libraries(drive-engine.measure PRIVATE drive-engine supple::core-analysis supple::doctest-main)
  supple_target_defaults(drive-engine.measure)
  target_compile_definitions(drive-engine.measure PRIVATE DRIVE_MEASURE_SWEEPS
    DRIVE_GUIDE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../../skill/guides/drive.md")
  add_executable(drive-bench EXCLUDE_FROM_ALL bench/drive_bench.cpp src/processor.cpp
    ../../../libs/core-dsp/src/oversampler.cpp)
  target_link_libraries(drive-bench PRIVATE drive-engine)
  supple_target_defaults(drive-bench)
  target_compile_options(drive-bench PRIVATE -O2)
endif()
