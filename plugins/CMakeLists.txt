add_subdirectory(common)

if(SUPPLE_BUILD_PLUGINS)
  include(ExternalProject)
  function(supple_add_plugin dir)
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
        -DPROBESYNTH_BUILD_GUI=${SUPPLE_BUILD_GUI}
        -DPROBESYNTH_VST3=${SUPPLE_PLUGIN_VST3}
        -DPROBESYNTH_RTSAN=${plugin_rtsan}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${dir}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
    endif()
  endfunction()
  supple_add_plugin(probe-synth)
endif()
