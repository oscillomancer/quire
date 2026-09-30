cmake_minimum_required(VERSION 3.21)
if (NOT SUPPLE_ROOT)
    get_filename_component(SUPPLE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

# This must be set before project(): project() creates the cache entry, and a
# later non-FORCE assignment leaves it empty. macOS 11 is the first release that
# can run the arm64 slice produced by current Apple Silicon builds.
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

if (APPLE)
    project(eq VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(eq VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_eq_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _eq_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(EQ_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(EQ_BUILD_TOOLS "Build tests and integration probes" ON)
option(EQ_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(EQ_STRICT_WARNINGS "Enable warnings on eq-owned code" ON)
option(EQ_SANITIZERS "Enable AddressSanitizer and UBSan on eq-owned code" OFF)
option(EQ_RTSAN "Instrument the SDK-free DSP test with RealtimeSanitizer" OFF)

function(eq_target_defaults target)
    if (EQ_STRICT_WARNINGS)
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
    if (EQ_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if (SUPPLE_SANITIZE STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

include(${SUPPLE_ROOT}/plugins/common/cmake/LinkMonorepo.cmake)
supple_plugin_add_monorepo_libraries()
set(SUPPLE_BUILD_TESTS OFF)
include(identity.cmake)
add_subdirectory("${SUPPLE_ROOT}/libs/core-analysis" "${CMAKE_BINARY_DIR}/supple/core-analysis")
target_include_directories(product-info INTERFACE "${CMAKE_BINARY_DIR}/generated")

add_subdirectory(engine "${CMAKE_BINARY_DIR}/supple/eq-engine")
add_library(eq-dsp INTERFACE)

target_link_libraries(eq-dsp INTERFACE supple::eq-engine)

if (EQ_BUILD_TOOLS)
    enable_testing()
    add_executable(eq-test test/test_core.cpp)
    target_link_libraries(eq-test PRIVATE eq-dsp)
    eq_target_defaults(eq-test)
    if (EQ_RTSAN)
        target_compile_options(eq-test PRIVATE -fsanitize=realtime)
        target_link_options(eq-test PRIVATE -fsanitize=realtime)
    endif()
    add_test(NAME core COMMAND eq-test)
    if (UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        add_executable(eq-semantic-rt test/semantic_rt.cpp)
        target_link_libraries(eq-semantic-rt PRIVATE
            supple::plugin-support Threads::Threads ${CMAKE_DL_LIBS})
        eq_target_defaults(eq-semantic-rt)
        if (EQ_RTSAN)
            target_compile_options(eq-semantic-rt PRIVATE -fsanitize=realtime)
            target_link_options(eq-semantic-rt PRIVATE -fsanitize=realtime)
        endif()
        add_test(NAME semantic-rt COMMAND eq-semantic-rt
            ${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.clap)
    endif()

    if (SUPPLE_SEMANTIC_CONFORMANCE)
        add_test(NAME semantic-conformance COMMAND ${SUPPLE_SEMANTIC_CONFORMANCE}
            ${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.clap com.suppleaudio.eq)
    endif()
endif()

function(eq_add_gui_preview)
    if (TARGET eq-gui-gl)
        set(preview_gui eq-gui-gl)
    else()
        set(preview_gui eq-gui)
    endif()
    add_executable(eq-gui-preview tools/gui_preview.cpp)
    target_link_libraries(eq-gui-preview PRIVATE ${preview_gui})
    target_include_directories(eq-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (EQ_BUILD_PLUGIN)
    include(FetchContent)
    # Patch validation needs Git even when dependency sources are overridden.
    find_package(Git REQUIRED)
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
    set(CLAP_SDK_ROOT "${SUPPLE_ROOT}/third_party/clap" CACHE PATH "" FORCE)
    set(FETCHCONTENT_SOURCE_DIR_CLAP-WRAPPER "${SUPPLE_ROOT}/third_party/clap-wrapper" CACHE PATH "" FORCE)
    set(FETCHCONTENT_SOURCE_DIR_PUGL "${SUPPLE_ROOT}/third_party/pugl" CACHE PATH "" FORCE)
    set(FETCHCONTENT_SOURCE_DIR_NANOVG "${SUPPLE_ROOT}/third_party/nanovg" CACHE PATH "" FORCE)
    set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(clap-wrapper)

    # The vendored wrapper carries both template VST3 patches. Never mutate it.
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-all-points.patch
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE points_present)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-timer-floor.patch
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE timer_present)
    if (NOT points_present EQUAL 0 OR NOT timer_present EQUAL 0)
        message(FATAL_ERROR "Vendored clap-wrapper lacks template VST3 patches")
    endif()

    if (EQ_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(eq-impl STATIC plugin/src/eq_clap.cpp)
    target_link_libraries(eq-impl PUBLIC clap clap-wrapper-extensions eq-dsp supple::plugin-support)
    if (EQ_RTSAN)
        target_compile_options(eq-impl PRIVATE -fsanitize=realtime)
    endif()
    eq_target_defaults(eq-impl)

    if (EQ_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(eq-impl PUBLIC eq-gui)
        target_compile_definitions(eq-impl PUBLIC EQ_WITH_GUI=1)
        if (EQ_BUILD_TOOLS)
            eq_add_gui_preview()
            add_executable(eq-editor-test test/editor_input.cpp)
            target_link_libraries(eq-editor-test PRIVATE supple::plugin-support
                $<IF:$<TARGET_EXISTS:eq-gui-gl>,eq-gui-gl,eq-gui>)
            target_include_directories(eq-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND eq-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(eq-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(eq-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(eq-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(eq-clap-probe PRIVATE clap eq-dsp)
            target_include_directories(eq-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(eq-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(eq-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(eq-impl PUBLIC EQ_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (EQ_BUILD_TOOLS)
        add_executable(eq-host-smoke test/host_smoke.cpp)
        target_link_libraries(eq-host-smoke PRIVATE clap)
        if (EQ_RTSAN)
            target_link_options(eq-host-smoke PRIVATE -fsanitize=realtime)
        endif()
        target_compile_definitions(eq-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.eq"
            HOST_SMOKE_PLAIN_PARAM="Output")
        eq_target_defaults(eq-host-smoke)
        add_test(NAME host-smoke
                 COMMAND eq-host-smoke
                         ${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.clap)
        add_test(NAME host-smoke-mono
                 COMMAND eq-host-smoke
                         ${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.clap --mono)
    endif()

    option(EQ_VST3 "Build VST3 format" OFF)
    set(EQ_FORMATS CLAP)
    if (EQ_VST3)
        list(APPEND EQ_FORMATS VST3)
    endif()
    if (APPLE)
        list(APPEND EQ_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME eq-plugin
        IMPL_TARGET eq-impl
        OUTPUT_NAME "${EQ_DISPLAY_NAME}"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/eq_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.eq"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${EQ_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../clap
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QEq_"
        AUV2_INSTRUMENT_TYPE "aumu")
    if (EQ_RTSAN AND TARGET eq-plugin_clap)
        target_link_options(eq-plugin_clap PRIVATE -fsanitize=realtime)
    elseif (SUPPLE_SANITIZE STREQUAL "thread" AND TARGET eq-plugin_clap)
        target_link_options(eq-plugin_clap PRIVATE -fsanitize=thread)
    endif()

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
    set(eq_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(eq_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(eq-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "eq-plugin_clap:${EQ_DISPLAY_NAME}.clap" "eq-plugin_vst3:${EQ_DISPLAY_NAME}.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(eq-notices ${target})
                add_custom_command(TARGET eq-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_notices}"
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_license}"
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET eq-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_notices}"
                "${CMAKE_BINARY_DIR}/../clap/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_license}"
                "${CMAKE_BINARY_DIR}/../clap/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET eq-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(eq-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/eq-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_notices}"
                "${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${eq_license}"
                "${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component"
            DEPENDS eq-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${EQ_DISPLAY_NAME}.component"
            VERBATIM)
        if (EQ_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${CMAKE_BINARY_DIR}/../clap/${EQ_DISPLAY_NAME}.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(EQ_INSTALL_SPECS
            "${EQ_DISPLAY_NAME}.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:eq-plugin_clap:dir"
            "${EQ_DISPLAY_NAME}.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:eq-plugin_vst3:dir"
            "${EQ_DISPLAY_NAME}.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:eq-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(EQ_INSTALL_SPECS
            "${EQ_DISPLAY_NAME}.clap:$ENV{HOME}/.clap:eq-plugin_clap:file"
            "${EQ_DISPLAY_NAME}.vst3:$ENV{HOME}/.vst3:eq-plugin_vst3:dir")
    endif()

    if (EQ_INSTALL_SPECS)
        add_custom_target(eq-install COMMENT "Installing every Eq format")
        if (TARGET eq-prepare-auv2)
            add_dependencies(eq-install eq-prepare-auv2)
        endif()
        foreach(spec ${EQ_INSTALL_SPECS})
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
                add_dependencies(eq-install ${target})
                add_custom_command(TARGET eq-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET eq-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
