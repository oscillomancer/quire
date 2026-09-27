add_subdirectory(common)
add_subdirectory(orrery/engine)
add_subdirectory(orrery/clap)

if(SUPPLE_BUILD_PLUGINS)
  include(ExternalProject)
  option(SUPPLE_PLUGIN_VST3 "Also build VST3 versions of the stock plugins" OFF)
  # supple_add_plugin(<dir> <PREFIX> <artifact>): builds plugins/<dir> (a template-derived
  # plugin, ADR-0011) as an isolated ExternalProject. PREFIX is the plugin's
  # CMake option prefix (the template's STARTER after renaming), so the
  # monorepo's settings map onto <PREFIX>_BUILD_GUI, <PREFIX>_VST3 and
  # <PREFIX>_RTSAN. The plugin's own ctest suite becomes plugin.<dir>.ctest.
  function(supple_add_plugin dir prefix artifact)
    set(plugin_build "${CMAKE_BINARY_DIR}/plugins/${dir}")
    if(SUPPLE_SANITIZE STREQUAL "realtime")
      set(plugin_rtsan ON)
      set(plugin_core_sanitize realtime)
    elseif(SUPPLE_SANITIZE STREQUAL "thread")
      set(plugin_rtsan OFF)
      set(plugin_core_sanitize thread)
    else()
      set(plugin_rtsan OFF)
      set(plugin_core_sanitize "")
    endif()
    ExternalProject_Add(plugin-${dir}
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
      BINARY_DIR "${plugin_build}"
      DOWNLOAD_COMMAND ""
      UPDATE_COMMAND ""
      BUILD_ALWAYS TRUE
      INSTALL_COMMAND ""
      CMAKE_GENERATOR Ninja
      CMAKE_ARGS
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DSUPPLE_ROOT=${CMAKE_SOURCE_DIR}
        -DSUPPLE_SEMANTIC_CONFORMANCE=${CMAKE_BINARY_DIR}/plugins/common/semantic-conformance
        -D${prefix}_BUILD_GUI=${SUPPLE_BUILD_GUI}
        -D${prefix}_VST3=${SUPPLE_PLUGIN_VST3}
        -D${prefix}_RTSAN=${plugin_rtsan}
        -DSUPPLE_SANITIZE=${plugin_core_sanitize}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${dir}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
      if(NOT SUPPLE_SANITIZE)
        add_test(NAME plugin.${dir}.validator
          COMMAND "${CMAKE_SOURCE_DIR}/tools/validator/run.sh"
                  "${CMAKE_BINARY_DIR}/plugins/clap/${artifact}.clap")
        set_tests_properties(plugin.${dir}.validator PROPERTIES
          LABELS slow SKIP_RETURN_CODE 77)
      endif()
    endif()
  endfunction()
  supple_add_plugin(probe-synth PROBESYNTH ProbeSynth)
  supple_add_plugin(orrery ORRERY Orrery)
endif()
