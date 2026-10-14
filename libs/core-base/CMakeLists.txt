supple_add_library(core-base
  SOURCES src/paths.cpp src/waitable.cpp src/rt.cpp src/main_thread_wake.cpp src/thread.cpp src/result.cpp src/chars.cpp src/units.cpp src/closest.cpp src/files.cpp src/sha256.cpp src/md5.cpp
  DEPS supple::product-info PRIVATE_DEPS supple::fast-float)

if(WIN32)
  target_link_libraries(core-base PRIVATE Synchronization)
endif()

supple_add_tests(core-base SOURCES tests/test_paths.cpp tests/test_waitable.cpp tests/test_result.cpp tests/test_chars.cpp tests/test_rt_primitives.cpp tests/test_main_thread_wake.cpp tests/test_thread.cpp tests/test_files.cpp tests/test_environment.cpp tests/test_sha256.cpp tests/test_md5.cpp LABELS rt)
