supple_add_library(core-dsp SOURCES src/dsp.cpp src/oversampler.cpp DEPS supple::core-base PRIVATE_DEPS supple::pocketfft)
# The analysis suites and optimizer runs spend most of their time in this
# library's filters and true-peak upsampler: unoptimized, core-analysis.tests
# runs six times slower. MATCHES, so AppleClang and clang-cl get it too.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT SUPPLE_SANITIZE)
  supple_compile_options(core-dsp PRIVATE -O2)
endif()
supple_add_tests(core-dsp SOURCES tests/test_dsp.cpp tests/test_oversampler.cpp LABELS rt)
