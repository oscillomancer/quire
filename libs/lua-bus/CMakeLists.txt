# Embedded Lua scripting on the command bus (ADR-0006). Model thread only; the
# Lua state is vendored (third_party/lua-5.4.9) and statically linked.
supple_add_library(lua-bus
  SOURCES src/script.cpp src/conversion.cpp src/runtime.cpp
  DEPS supple::command-bus supple::core-schema supple::lua)

if(SUPPLE_BUILD_TESTS)
  # tsan-skip: ThreadSanitizer's longjmp interceptor cannot follow Lua's
  # setjmp/longjmp error handling ("can't find longjmp buf"), which every
  # pcall path here uses. asan/ubsan cover this library.
  supple_add_tests(lua-bus SOURCES tests/test_script.cpp tests/test_runtime.cpp LABELS tsan-skip)
endif()
