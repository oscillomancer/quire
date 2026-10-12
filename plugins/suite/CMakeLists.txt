# The suite layer (ADR-0032): what turns a product of the plugin template
# into a member of the suite. Here: the DSP script runtime and the script
# compiler. Both build without the rest of the monorepo, as the player and
# source exports need.

set(SUPPLE_TEMPLATE_FRAMEWORK ${CMAKE_SOURCE_DIR}/plugins/template/framework)

# The runtime: <supple/script.h> and its implementation over the framework's
# DSP library.
add_library(script-runtime STATIC runtime/src/runtime.cpp)
target_include_directories(script-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include)
target_include_directories(script-runtime SYSTEM PRIVATE ${SUPPLE_TEMPLATE_FRAMEWORK})
set_target_properties(script-runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
supple_target_defaults(script-runtime)
add_library(supple::script-runtime ALIAS script-runtime)

# The compiler: front end and MIR code generation (supple/script_compiler.hpp).
add_library(script-compiler STATIC
  compiler/src/lexer.cpp
  compiler/src/parser.cpp
  compiler/src/builtins.cpp
  compiler/src/sema.cpp
  compiler/src/sema_expr.cpp
  compiler/src/codegen.cpp
  compiler/src/compiler.cpp)
target_include_directories(script-compiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/compiler/include
                                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compiler/src)
target_link_libraries(script-compiler PUBLIC supple::script-runtime PRIVATE supple::mir)
set_target_properties(script-compiler PROPERTIES POSITION_INDEPENDENT_CODE ON)
supple_target_defaults(script-compiler)
add_library(supple::script-compiler ALIAS script-compiler)

if(SUPPLE_BUILD_TESTS)
  add_executable(script-compiler.tests compiler/tests/test_compiler.cpp)
  target_include_directories(script-compiler.tests PRIVATE compiler/tests)
  target_link_libraries(script-compiler.tests PRIVATE supple::script-compiler supple::doctest-main)
  supple_target_defaults(script-compiler.tests)
  add_test(NAME script-compiler.tests COMMAND script-compiler.tests)
  set_tests_properties(script-compiler.tests PROPERTIES LABELS "script-compiler")
endif()

if(SUPPLE_BUILD_TESTS)
  # The examples compiled as C++, as a source export builds them: warnings
  # are the script author's business (not -Werror), signed overflow wraps.
  add_executable(script-examples.tests compiler/tests/test_examples.cpp)
  target_include_directories(script-examples.tests PRIVATE compiler/tests examples)
  target_link_libraries(script-examples.tests PRIVATE supple::script-compiler supple::doctest-main)
  target_compile_features(script-examples.tests PRIVATE cxx_std_20)
  target_compile_options(script-examples.tests PRIVATE -ffp-contract=off -fwrapv ${_supple_sanitize_flags})
  target_link_options(script-examples.tests PRIVATE ${_supple_sanitize_flags})
  target_compile_definitions(script-examples.tests PRIVATE SUPPLE_SCRIPT_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  add_test(NAME script-examples.tests COMMAND script-examples.tests)
  set_tests_properties(script-examples.tests PROPERTIES LABELS "script-compiler")
endif()
