set(platform_sources)
set(platform_tests)
if(SUPPLE_PLATFORM_SERVICES)
  list(APPEND platform_sources src/rt.cpp src/main_thread_wake.cpp src/thread.cpp)
  list(APPEND platform_tests tests/test_main_thread_wake.cpp tests/test_thread.cpp)
endif()
supple_add_library(core-base
  SOURCES ${platform_sources} src/result.cpp src/units.cpp src/closest.cpp src/files.cpp src/sha256.cpp src/md5.cpp
  DEPS supple::product-info)

supple_add_tests(core-base SOURCES tests/test_result.cpp tests/test_rt_primitives.cpp ${platform_tests} tests/test_files.cpp tests/test_environment.cpp tests/test_sha256.cpp tests/test_md5.cpp LABELS rt)
