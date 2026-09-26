add_subdirectory(common)
add_subdirectory(orrery/engine)
add_subdirectory(orrery/clap)

if(SUPPLE_BUILD_PLUGINS)
  include(ExternalProject)
  option(SUPPLE_PLUGIN_VST3 "Also build VST3 versions of the stock plugins" OFF)
  # supple_add_plugin(<dir> <PREFIX>): builds plugins/<dir> (a template-derived
  # plugin, ADR-0011) as an isolated ExternalProject. PREFIX is the plugin's
  # CMake option prefix (the template's STARTER after renaming), so the
  # monorepo's settings map onto <PREFIX>_BUILD_GUI, <PREFIX>_VST3 and
  # <PREFIX>_RTSAN. The plugin's own ctest suite becomes plugin.<dir>.ctest.
  function(supple_add_plugin dir prefix)
    set(plugin_build "${CMAKE_BINARY_DIR}/plugins/${dir}")
    if(SUPPLE_SANITIZE STREQUAL "realtime")
      set(plugin_rtsan ON)
    else()
      set(plugin_rtsan OFF)
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
        -D${prefix}_BUILD_GUI=${SUPPLE_BUILD_GUI}
        -D${prefix}_VST3=${SUPPLE_PLUGIN_VST3}
        -D${prefix}_RTSAN=${plugin_rtsan}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${dir}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
    endif()
  endfunction()
  supple_add_plugin(probe-synth PROBESYNTH)
endif()
