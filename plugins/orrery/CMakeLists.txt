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
    project(orrery VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(orrery VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_orrery_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _orrery_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(ORRERY_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(ORRERY_BUILD_TOOLS "Build tests and integration probes" ON)
option(ORRERY_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(ORRERY_STRICT_WARNINGS "Enable warnings on orrery-owned code" ON)
option(ORRERY_SANITIZERS "Enable AddressSanitizer and UBSan on orrery-owned code" OFF)
option(ORRERY_RTSAN "Instrument the plugin and its tests with RealtimeSanitizer" OFF)

function(orrery_target_defaults target)
    if (ORRERY_STRICT_WARNINGS)
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
    if (ORRERY_SANITIZERS AND NOT MSVC)
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

# Orrery's SDK-free engine and its CLAP core live beside this template copy
# (plugins/orrery/engine, plugins/orrery/clap) and are built and tested by the
# monorepo; this isolated project builds the same sources for the plugin.
set(SUPPLE_BUILD_TESTS OFF)
add_subdirectory(${SUPPLE_ROOT}/plugins/orrery/engine ${CMAKE_BINARY_DIR}/supple/orrery-engine)
add_subdirectory(${SUPPLE_ROOT}/plugins/orrery/clap ${CMAKE_BINARY_DIR}/supple/orrery-clap)
if (ORRERY_RTSAN)
    foreach(t orrery-engine orrery-clap)
        target_compile_options(${t} PRIVATE -fsanitize=realtime)
    endforeach()
endif()

if (ORRERY_BUILD_TOOLS)
    enable_testing()
    if (SUPPLE_SEMANTIC_CONFORMANCE)
        add_test(NAME semantic-conformance COMMAND ${SUPPLE_SEMANTIC_CONFORMANCE}
            ${CMAKE_BINARY_DIR}/../clap/Orrery.clap com.suppleaudio.orrery)
    endif()
endif()

function(orrery_add_gui_preview)
    if (TARGET orrery-gui-gl)
        set(preview_gui orrery-gui-gl)
    else()
        set(preview_gui orrery-gui)
    endif()
    add_executable(orrery-gui-preview tools/gui_preview.cpp)
    target_link_libraries(orrery-gui-preview PRIVATE ${preview_gui})
    target_include_directories(orrery-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (ORRERY_BUILD_PLUGIN)
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

    if (ORRERY_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(orrery-impl STATIC plugin/src/orrery_plugin.cpp)
    target_link_libraries(orrery-impl PUBLIC clap clap-wrapper-extensions supple::orrery-clap)
    if (ORRERY_RTSAN)
        target_compile_options(orrery-impl PRIVATE -fsanitize=realtime)
    endif()
    orrery_target_defaults(orrery-impl)

    if (ORRERY_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(orrery-impl PUBLIC orrery-gui)
        target_compile_definitions(orrery-impl PUBLIC ORRERY_WITH_GUI=1)
        if (ORRERY_BUILD_TOOLS)
            orrery_add_gui_preview()
            add_executable(orrery-editor-test test/editor_input.cpp)
            target_link_libraries(orrery-editor-test PRIVATE supple::orrery-clap
                $<IF:$<TARGET_EXISTS:orrery-gui-gl>,orrery-gui-gl,orrery-gui>)
            orrery_target_defaults(orrery-editor-test)
            target_include_directories(orrery-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND orrery-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(orrery-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(orrery-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(orrery-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(orrery-clap-probe PRIVATE clap)
            target_include_directories(orrery-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(orrery-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(orrery-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(orrery-impl PUBLIC ORRERY_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (ORRERY_BUILD_TOOLS)
        add_executable(orrery-host-smoke test/host_smoke.cpp)
        target_link_libraries(orrery-host-smoke PRIVATE clap)
        if (ORRERY_RTSAN)
            target_link_options(orrery-host-smoke PRIVATE -fsanitize=realtime)
        endif()
        target_compile_definitions(orrery-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.orrery"
            HOST_SMOKE_PLAIN_PARAM="Voice Drift")
        orrery_target_defaults(orrery-host-smoke)
        add_test(NAME host-smoke
                 COMMAND orrery-host-smoke
                         ${CMAKE_BINARY_DIR}/../clap/Orrery.clap)
        add_test(NAME host-smoke-mono
                 COMMAND orrery-host-smoke
                         ${CMAKE_BINARY_DIR}/../clap/Orrery.clap --mono)
    endif()

    option(ORRERY_VST3 "Build VST3 format" OFF)
    set(ORRERY_FORMATS CLAP)
    if (ORRERY_VST3)
        list(APPEND ORRERY_FORMATS VST3)
    endif()
    if (APPLE)
        list(APPEND ORRERY_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME orrery-plugin
        IMPL_TARGET orrery-impl
        OUTPUT_NAME "Orrery"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/orrery_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.orrery"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${ORRERY_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../clap
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QOry"
        AUV2_INSTRUMENT_TYPE "aumu")
    if (ORRERY_RTSAN AND TARGET orrery-plugin_clap)
        target_link_options(orrery-plugin_clap PRIVATE -fsanitize=realtime)
    elseif (SUPPLE_SANITIZE STREQUAL "thread" AND TARGET orrery-plugin_clap)
        target_link_options(orrery-plugin_clap PRIVATE -fsanitize=thread)
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
    set(orrery_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(orrery_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(orrery-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "orrery-plugin_clap:Orrery.clap" "orrery-plugin_vst3:Orrery.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(orrery-notices ${target})
                add_custom_command(TARGET orrery-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_notices}"
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_license}"
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET orrery-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_notices}"
                "${CMAKE_BINARY_DIR}/../clap/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_license}"
                "${CMAKE_BINARY_DIR}/../clap/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET orrery-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(orrery-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/orrery-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${CMAKE_BINARY_DIR}/../clap/Orrery.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/../clap/Orrery.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_notices}"
                "${CMAKE_BINARY_DIR}/../clap/Orrery.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${orrery_license}"
                "${CMAKE_BINARY_DIR}/../clap/Orrery.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${CMAKE_BINARY_DIR}/../clap/Orrery.component"
            DEPENDS orrery-plugin_auv2
            COMMENT "Preparing and ad-hoc signing Orrery.component"
            VERBATIM)
        if (ORRERY_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${CMAKE_BINARY_DIR}/../clap/Orrery.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(ORRERY_INSTALL_SPECS
            "Orrery.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:orrery-plugin_clap:dir"
            "Orrery.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:orrery-plugin_vst3:dir"
            "Orrery.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:orrery-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(ORRERY_INSTALL_SPECS
            "Orrery.clap:$ENV{HOME}/.clap:orrery-plugin_clap:file"
            "Orrery.vst3:$ENV{HOME}/.vst3:orrery-plugin_vst3:dir")
    endif()

    if (ORRERY_INSTALL_SPECS)
        add_custom_target(orrery-install COMMENT "Installing every Orrery format")
        if (TARGET orrery-prepare-auv2)
            add_dependencies(orrery-install orrery-prepare-auv2)
        endif()
        foreach(spec ${ORRERY_INSTALL_SPECS})
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
                add_dependencies(orrery-install ${target})
                add_custom_command(TARGET orrery-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${CMAKE_BINARY_DIR}/../clap/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET orrery-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
