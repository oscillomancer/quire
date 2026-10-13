add_subdirectory(common)
add_subdirectory(suite)
add_subdirectory(gate/engine)
add_subdirectory(delay/engine)
add_subdirectory(analyzer/engine)
add_subdirectory(eq/engine)
add_subdirectory(orrery/engine)
add_subdirectory(utility/engine)
add_subdirectory(comp/engine)
add_subdirectory(drive/engine)
add_subdirectory(transient/engine)
add_subdirectory(reverb/engine)
add_subdirectory(orrery/clap)

if(SUPPLE_BUILD_PLUGINS)
  include(ExternalProject)
  option(SUPPLE_PLUGIN_VST3 "Also build VST3 versions of the stock plugins" OFF)
  set(VST3_SDK_ROOT "" CACHE PATH "Complete VST3 SDK for opt-in stock-plugin builds")
  # supple_add_plugin(<dir> <PREFIX> [artifact]): builds plugins/<dir> (a template-derived
  # plugin, ADR-0011) as an isolated ExternalProject. PREFIX is the plugin's
  # CMake option prefix (the template's STARTER after renaming), so the
  # monorepo's settings map onto <PREFIX>_BUILD_GUI, <PREFIX>_VST3 and
  # <PREFIX>_RTSAN. The plugin's own ctest suite becomes plugin.<dir>.ctest.
  function(supple_add_plugin dir prefix)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/cmake/Identity.cmake")
      include("${CMAKE_CURRENT_SOURCE_DIR}/${dir}/cmake/Identity.cmake")
      set(artifact "${${prefix}_DISPLAY_NAME}")
    elseif(ARGC GREATER 2)
      set(artifact "${ARGV2}")
    else()
      message(FATAL_ERROR "Plugin ${dir} needs cmake/Identity.cmake or an explicit artifact name")
    endif()
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
        -D${prefix}_ASSET_DIR=${CMAKE_BINARY_DIR}/plugins/clap
        -D${prefix}_NOTICES_PREFIX=${artifact}-
        -DSUPPLE_SEMANTIC_CONFORMANCE=${CMAKE_BINARY_DIR}/plugins/common/semantic-conformance
        -D${prefix}_BUILD_GUI=${SUPPLE_BUILD_GUI}
        -D${prefix}_VST3=${SUPPLE_PLUGIN_VST3}
        -DVST3_SDK_ROOT=${VST3_SDK_ROOT}
        -D${prefix}_RTSAN=${plugin_rtsan}
        -DSUPPLE_SANITIZE=${plugin_core_sanitize}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${dir}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
      if(UNIX AND NOT APPLE)
        add_test(NAME plugin.${dir}.unload COMMAND clap-unload-probe
          "${CMAKE_BINARY_DIR}/plugins/clap/${artifact}.clap")
        set_tests_properties(plugin.${dir}.unload PROPERTIES
          LABELS "rt;plugin-unload" TIMEOUT 30)
      endif()
      # A render after reset() must depend only on the state set for it: the
      # sandbox and the engine reuse one instance across renders.
      add_test(NAME plugin.${dir}.reset COMMAND clap-reset-check
        "${CMAKE_BINARY_DIR}/plugins/clap/${artifact}.clap")
      set_tests_properties(plugin.${dir}.reset PROPERTIES
        LABELS "rt;plugin-reset" TIMEOUT 180)
      if(NOT SUPPLE_SANITIZE)
        add_test(NAME plugin.${dir}.validator
          COMMAND "${CMAKE_SOURCE_DIR}/tools/validator/run.sh"
                  "${CMAKE_BINARY_DIR}/plugins/clap/${artifact}.clap")
        set_tests_properties(plugin.${dir}.validator PROPERTIES
          LABELS slow SKIP_RETURN_CODE 77)
      endif()
    endif()
  endfunction()
  # supple_add_template_product(<name> <product> <artifact>): a product of
  # the plugin template at framework 1.16.1 or later, built from the template's
  # tree (plugins/template) with plugins/<product> as its product folder, as
  # an isolated ExternalProject. Its dependencies are this repository's
  # third_party copies, at the template's pins; it makes the CLAP alone
  # unless SUPPLE_PLUGIN_VST3 (with a complete VST3_SDK_ROOT). The CLAP, its
  # license and notices are staged in plugins/clap beside the stock plugins.
  # Its own ctest suite becomes plugin.<name>.ctest.
  function(supple_add_template_product name product artifact)
    set(plugin_build "${CMAKE_BINARY_DIR}/plugins/${name}")
    # As for the stock plugins: instrumented for tsan and rtsan, whose hosts
    # are, and not for asan or ubsan.
    if(SUPPLE_SANITIZE STREQUAL "realtime" OR SUPPLE_SANITIZE STREQUAL "thread")
      set(plugin_sanitize ${SUPPLE_SANITIZE})
    else()
      set(plugin_sanitize "")
    endif()
    set(tp "${CMAKE_SOURCE_DIR}/third_party")
    ExternalProject_Add(plugin-${name}
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/template"
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
        -DSUPPLE_PRODUCT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/${product}
        -DSUPPLE_BUILD_GUI=${SUPPLE_BUILD_GUI}
        -DSUPPLE_BUILD_TESTS=${SUPPLE_BUILD_TESTS}
        -DSUPPLE_BUILD_VST3=${SUPPLE_PLUGIN_VST3}
        -DVST3_SDK_ROOT=${VST3_SDK_ROOT}
        -DSUPPLE_SANITIZE=${plugin_sanitize}
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
        -DFETCHCONTENT_SOURCE_DIR_CLAP=${tp}/clap
        -DFETCHCONTENT_SOURCE_DIR_CLAP-WRAPPER=${tp}/clap-wrapper
        -DFETCHCONTENT_SOURCE_DIR_DR_LIBS=${tp}/dr_libs/include
        -DFETCHCONTENT_SOURCE_DIR_PUGL=${tp}/pugl
        -DFETCHCONTENT_SOURCE_DIR_NANOVG=${tp}/nanovg
        -DFETCHCONTENT_SOURCE_DIR_NANOSVG=${tp}/nanosvg
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel 1)
    set(staged "${CMAKE_BINARY_DIR}/plugins/clap")
    ExternalProject_Add_Step(plugin-${name} stage
      COMMAND ${CMAKE_COMMAND} -E make_directory "${staged}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${plugin_build}/plugins/${artifact}.clap"
              "${plugin_build}/plugins/${artifact}-LICENSE.txt"
              "${plugin_build}/plugins/${artifact}-NOTICES.txt" "${staged}"
      DEPENDEES build
      ALWAYS TRUE)
    if(SUPPLE_BUILD_TESTS)
      add_test(NAME plugin.${name}.ctest
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${plugin_build}" --output-on-failure)
    endif()
    set(${name}_clap "${staged}/${artifact}.clap" PARENT_SCOPE)
  endfunction()

  supple_add_plugin(gate GATE)
  supple_add_plugin(probe-synth PROBESYNTH ProbeSynth)
  supple_add_plugin(orrery ORRERY Orrery)
  supple_add_plugin(utility UTILITY)
  supple_add_plugin(analyzer ANALYZER)
  supple_add_plugin(delay DELAY)
  supple_add_plugin(comp COMP)
  supple_add_plugin(eq EQ)
  supple_add_plugin(drive DRIVE)
  supple_add_plugin(transient TRANSIENT)
  supple_add_plugin(reverb REVERB)

  # The player runs DSP scripts (ADR-0032): its plugins are the scripts in
  # SUPPLE_SCRIPT_LIBRARY's folders, so the checks every stock plugin has run
  # with the player's test scripts (Smoke; Tilt, which keeps state; Pluck,
  # an instrument).
  supple_add_template_product(player suite/player "Script Player")
  if(SUPPLE_BUILD_TESTS)
    set(player_scripts "${CMAKE_CURRENT_SOURCE_DIR}/suite/player/test")
    set(player_env "SUPPLE_SCRIPT_LIBRARY=${player_scripts}/smoke:${player_scripts}/library")
    set(player_tests plugin.player.reset plugin.player.semantic.smoke plugin.player.semantic.tilt)
    add_test(NAME plugin.player.reset COMMAND clap-reset-check "${player_clap}")
    set_tests_properties(plugin.player.reset PROPERTIES LABELS "rt;plugin-reset" TIMEOUT 180)
    add_test(NAME plugin.player.semantic.smoke
      COMMAND semantic-conformance "${player_clap}" com.suppleaudio.script-player)
    add_test(NAME plugin.player.semantic.tilt COMMAND semantic-conformance "${player_clap}" com.example.tilt)
    add_test(NAME plugin.player.semantic.pluck COMMAND semantic-conformance "${player_clap}" com.example.pluck)
    list(APPEND player_tests plugin.player.semantic.pluck)
    if(UNIX AND NOT APPLE)
      add_test(NAME plugin.player.unload COMMAND clap-unload-probe "${player_clap}")
      set_tests_properties(plugin.player.unload PROPERTIES LABELS "rt;plugin-unload" TIMEOUT 30)
      list(APPEND player_tests plugin.player.unload)
    endif()
    if(NOT SUPPLE_SANITIZE)
      add_test(NAME plugin.player.validator
        COMMAND "${CMAKE_SOURCE_DIR}/tools/validator/run.sh" "${player_clap}")
      set_tests_properties(plugin.player.validator PROPERTIES LABELS slow SKIP_RETURN_CODE 77)
      list(APPEND player_tests plugin.player.validator)
    endif()
    set_tests_properties(${player_tests} PROPERTIES ENVIRONMENT "${player_env}")
    # A source export of the player's tilt script, built and tested as a
    # user would, then played against the player (suite/tests).
    add_test(NAME plugin.export.source
      COMMAND ${CMAKE_COMMAND} -DSANDBOX=$<TARGET_FILE:${PRODUCT_SLUG}-sandbox>
              -DPLAYER=${player_clap} -DKIT=${CMAKE_BINARY_DIR}/plugins/script-export
              -DSCRIPT=${player_scripts}/library/tilt/tilt.cpp
              "-DPATCH={\"tilt\": 4.5, \"pivot\": 700, \"output\": -2}"
              -DWORK=${CMAKE_BINARY_DIR}/plugins/export-test -DTHIRD_PARTY=${CMAKE_SOURCE_DIR}/third_party
              -DCC=${CMAKE_C_COMPILER} -DCXX=${CMAKE_CXX_COMPILER} -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/suite/tests/source_export.cmake)
    set_tests_properties(plugin.export.source PROPERTIES LABELS slow TIMEOUT 1800)
  endif()
endif()
