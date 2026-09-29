cmake_minimum_required(VERSION 3.21)

# This must be set before project(): project() creates the cache entry, and a
# later non-FORCE assignment leaves it empty. macOS 11 is the first release that
# can run the arm64 slice produced by current Apple Silicon builds.
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

if (APPLE)
    project(analyzer VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(analyzer VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_analyzer_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _analyzer_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(ANALYZER_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(ANALYZER_BUILD_TOOLS "Build tests and integration probes" ON)
option(ANALYZER_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(ANALYZER_STRICT_WARNINGS "Enable warnings on analyzer-owned code" ON)
option(ANALYZER_SANITIZERS "Enable AddressSanitizer and UBSan on analyzer-owned code" OFF)

function(analyzer_target_defaults target)
    if (ANALYZER_STRICT_WARNINGS)
        if (MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic)
            if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                # NanoVG's public color type intentionally uses this C extension.
                target_compile_options(${target} PRIVATE
                    -Wno-gnu-anonymous-struct -Wno-nested-anon-types)
            endif()
        endif()
    endif()
    if (ANALYZER_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(analyzer-dsp INTERFACE)
target_include_directories(analyzer-dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)

if (ANALYZER_BUILD_TOOLS)
    enable_testing()
    add_executable(analyzer-cli tools/analyzer_cli.cpp)
    target_link_libraries(analyzer-cli PRIVATE analyzer-dsp)
    analyzer_target_defaults(analyzer-cli)
    add_executable(analyzer-test test/test_core.cpp)
    target_link_libraries(analyzer-test PRIVATE analyzer-dsp)
    analyzer_target_defaults(analyzer-test)
    add_test(NAME core COMMAND analyzer-test)
endif()

function(analyzer_add_gui_preview)
    if (TARGET analyzer-gui-gl)
        set(preview_gui analyzer-gui-gl)
    else()
        set(preview_gui analyzer-gui)
    endif()
    add_executable(analyzer-gui-preview tools/gui_preview.cpp)
    target_link_libraries(analyzer-gui-preview PRIVATE ${preview_gui})
    target_include_directories(analyzer-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (ANALYZER_BUILD_PLUGIN)
    include(FetchContent)
    # Patch validation needs Git even when dependency sources are overridden.
    find_package(Git REQUIRED)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(clap-wrapper)

    # clap-wrapper 0.16.0 keeps only the final VST3 automation point in each
    # parameter queue. Apply the small upstreamable fix after population; the
    # reverse check makes repeated configure runs idempotent.
    set(analyzer_wrapper_vst3_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-all-points.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${analyzer_wrapper_vst3_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_patch_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_patch_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${analyzer_wrapper_vst3_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${analyzer_wrapper_vst3_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_patch_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_patch_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 automation patch no longer applies")
        endif()
    endif()

    # register_timer() clamps every request to 30ms upstream, capping a VST3
    # editor at 33fps however fast it asked to run. The AUv2 path returns false
    # instead and falls through to the plugin's own timer at the full 60Hz,
    # which is why an AU looks smoother than the VST3 of the same build.
    set(analyzer_wrapper_vst3_patch_timer ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-timer-floor.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${analyzer_wrapper_vst3_patch_timer}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_timer_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_timer_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${analyzer_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${analyzer_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_timer_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_timer_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 timer floor patch no longer applies")
        endif()
    endif()

    if (ANALYZER_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(analyzer-impl STATIC plugin/src/analyzer_clap.cpp)
    target_link_libraries(analyzer-impl PUBLIC clap clap-wrapper-extensions analyzer-dsp)
    analyzer_target_defaults(analyzer-impl)

    if (ANALYZER_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(analyzer-impl PUBLIC analyzer-gui)
        target_compile_definitions(analyzer-impl PUBLIC ANALYZER_WITH_GUI=1)
        if (ANALYZER_BUILD_TOOLS)
            analyzer_add_gui_preview()
            add_executable(analyzer-editor-test test/editor_input.cpp)
            target_link_libraries(analyzer-editor-test PRIVATE
                $<IF:$<TARGET_EXISTS:analyzer-gui-gl>,analyzer-gui-gl,analyzer-gui>)
            target_include_directories(analyzer-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND analyzer-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(analyzer-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(analyzer-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(analyzer-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(analyzer-clap-probe PRIVATE clap)
            target_include_directories(analyzer-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(analyzer-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(analyzer-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(analyzer-impl PUBLIC ANALYZER_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (ANALYZER_BUILD_TOOLS)
        add_executable(analyzer-host-smoke test/host_smoke.cpp)
        target_link_libraries(analyzer-host-smoke PRIVATE clap)
        target_compile_definitions(analyzer-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.analyzer"
            HOST_SMOKE_PLAIN_PARAM="Input Gain")
        analyzer_target_defaults(analyzer-host-smoke)
        add_test(NAME host-smoke
                 COMMAND analyzer-host-smoke
                         ${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.clap)
        add_test(NAME host-smoke-mono
                 COMMAND analyzer-host-smoke
                         ${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.clap --mono)
    endif()

    set(ANALYZER_FORMATS CLAP VST3)
    if (APPLE)
        list(APPEND ANALYZER_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME analyzer-plugin
        IMPL_TARGET analyzer-impl
        OUTPUT_NAME "Analyzer"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/analyzer_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.analyzer"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${ANALYZER_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/analyzer_assets
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QAna"
        AUV2_INSTRUMENT_TYPE "aufx")

    # The license and attribution have to travel with the binary. Files sitting
    # in the repository do nothing for someone who is handed only a bundle, and
    # both the product license and every dependency in the stack require their
    # text to accompany what is distributed. The install target copies whole bundles, so anything placed
    # inside one is carried along for free.
    #
    # This is a target of its own rather than a POST_BUILD on each plugin: a
    # POST_BUILD only runs when its target relinks, so editing NOTICES.txt alone
    # would leave the bundles holding the previous copy. That is the same
    # staleness that already bit the AU's Info.plist, and a custom target is
    # always out of date, so the copy is reconsidered every build.
    set(analyzer_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(analyzer_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(analyzer-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "analyzer-plugin_clap:Analyzer.clap" "analyzer-plugin_vst3:Analyzer.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(analyzer-notices ${target})
                add_custom_command(TARGET analyzer-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_BINARY_DIR}/analyzer_assets/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_notices}"
                        "${CMAKE_BINARY_DIR}/analyzer_assets/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_license}"
                        "${CMAKE_BINARY_DIR}/analyzer_assets/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET analyzer-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_notices}"
                "${CMAKE_BINARY_DIR}/analyzer_assets/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_license}"
                "${CMAKE_BINARY_DIR}/analyzer_assets/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET analyzer-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(analyzer-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/analyzer-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_notices}"
                "${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${analyzer_license}"
                "${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component"
            DEPENDS analyzer-plugin_auv2
            COMMENT "Preparing and ad-hoc signing Analyzer.component"
            VERBATIM)
        if (ANALYZER_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${CMAKE_BINARY_DIR}/analyzer_assets/Analyzer.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(ANALYZER_INSTALL_SPECS
            "Analyzer.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:analyzer-plugin_clap:dir"
            "Analyzer.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:analyzer-plugin_vst3:dir"
            "Analyzer.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:analyzer-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(ANALYZER_INSTALL_SPECS
            "Analyzer.clap:$ENV{HOME}/.clap:analyzer-plugin_clap:file"
            "Analyzer.vst3:$ENV{HOME}/.vst3:analyzer-plugin_vst3:dir")
    endif()

    if (ANALYZER_INSTALL_SPECS)
        add_custom_target(analyzer-install COMMENT "Installing every Analyzer format")
        if (TARGET analyzer-prepare-auv2)
            add_dependencies(analyzer-install analyzer-prepare-auv2)
        endif()
        foreach(spec ${ANALYZER_INSTALL_SPECS})
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 bundle)
            list(GET parts 1 destination)
            list(GET parts 2 target)
            list(GET parts 3 kind)
            if (kind STREQUAL "dir")
                set(copyCmd copy_directory)
            else()
                set(copyCmd copy)
            endif()
            if (TARGET ${target})
                add_dependencies(analyzer-install ${target})
                add_custom_command(TARGET analyzer-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${CMAKE_BINARY_DIR}/analyzer_assets/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET analyzer-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
