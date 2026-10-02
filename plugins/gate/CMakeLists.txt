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
    project(gate VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(gate VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_gate_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _gate_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(GATE_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(GATE_BUILD_TOOLS "Build tests and integration probes" ON)
option(GATE_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(GATE_STRICT_WARNINGS "Enable warnings on gate-owned code" ON)
option(GATE_SANITIZERS "Enable AddressSanitizer and UBSan on gate-owned code" OFF)
option(GATE_RTSAN "Instrument the SDK-free DSP test with RealtimeSanitizer" OFF)
# Where the built formats are written (the template's layout); the Quire
# monorepo build passes its shared plugins/clap folder instead.
set(GATE_ASSET_DIR "${CMAKE_BINARY_DIR}/gate_assets" CACHE PATH
    "Directory the plugin formats are written to")

function(gate_target_defaults target)
    if (GATE_STRICT_WARNINGS)
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
    if (GATE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if (SUPPLE_SANITIZE STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
    if (SUPPLE_SANITIZE STREQUAL "address")
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
    elseif (SUPPLE_SANITIZE STREQUAL "undefined")
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
endfunction()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Identity.cmake)
include(${SUPPLE_ROOT}/plugins/common/cmake/LinkMonorepo.cmake)
supple_plugin_add_monorepo_libraries()

set(SUPPLE_BUILD_TESTS OFF)
add_subdirectory(engine ${CMAKE_BINARY_DIR}/supple/gate-engine)
set(SUPPLE_BUILD_TESTS ON)
add_library(gate-dsp INTERFACE)
target_link_libraries(gate-dsp INTERFACE supple::gate-engine)

if (GATE_BUILD_TOOLS)
    enable_testing()
    add_executable(gate-test test/test_core.cpp)
    target_link_libraries(gate-test PRIVATE gate-dsp)
    gate_target_defaults(gate-test)
    if (GATE_RTSAN)
        target_compile_options(gate-test PRIVATE -fsanitize=realtime)
        target_link_options(gate-test PRIVATE -fsanitize=realtime)
    endif()
    add_test(NAME core COMMAND gate-test)
    if (UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        add_executable(gate-semantic-rt test/semantic_rt.cpp)
        target_link_libraries(gate-semantic-rt PRIVATE
            supple::plugin-support gate-dsp Threads::Threads ${CMAKE_DL_LIBS})
        gate_target_defaults(gate-semantic-rt)
        if (GATE_RTSAN)
            target_compile_options(gate-semantic-rt PRIVATE -fsanitize=realtime)
            target_link_options(gate-semantic-rt PRIVATE -fsanitize=realtime)
        endif()
        add_test(NAME semantic-rt COMMAND gate-semantic-rt
            ${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.clap)
    endif()

    if (SUPPLE_SEMANTIC_CONFORMANCE)
        add_test(NAME semantic-conformance COMMAND ${SUPPLE_SEMANTIC_CONFORMANCE}
            ${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.clap com.suppleaudio.gate)
    endif()
endif()

function(gate_add_gui_preview)
    if (TARGET gate-gui-gl)
        set(preview_gui gate-gui-gl)
    else()
        set(preview_gui gate-gui)
    endif()
    add_executable(gate-gui-preview tools/gui_preview.cpp)
    target_link_libraries(gate-gui-preview PRIVATE ${preview_gui})
    target_include_directories(gate-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (GATE_BUILD_PLUGIN)
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

    # The vendored wrapper carries all three template VST3 patches. Never mutate it.
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-all-points.patch
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE points_present)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-timer-floor.patch
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE timer_present)
    set(gate_wrapper_self_handle_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-self-handle.patch)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${gate_wrapper_self_handle_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE self_handle_present)
    if (NOT points_present EQUAL 0 OR NOT timer_present EQUAL 0 OR NOT self_handle_present EQUAL 0)
        message(FATAL_ERROR "Vendored clap-wrapper lacks template VST3 patches")
    endif()

    if (GATE_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(gate-impl STATIC plugin/src/gate_clap.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(gate-impl PUBLIC clap clap-wrapper-extensions gate-dsp supple::plugin-support Threads::Threads)
    if (GATE_RTSAN)
        target_compile_options(gate-impl PRIVATE -fsanitize=realtime)
    endif()
    gate_target_defaults(gate-impl)

    if (GATE_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(gate-impl PUBLIC gate-gui)
        target_compile_definitions(gate-impl PUBLIC GATE_WITH_GUI=1)
        if (GATE_BUILD_TOOLS)
            gate_add_gui_preview()
            add_executable(gate-editor-test test/editor_input.cpp)
            target_link_libraries(gate-editor-test PRIVATE supple::plugin-support
                $<IF:$<TARGET_EXISTS:gate-gui-gl>,gate-gui-gl,gate-gui>)
            target_include_directories(gate-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND gate-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(gate-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(gate-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(gate-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(gate-clap-probe PRIVATE clap gate-dsp)
            target_include_directories(gate-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(gate-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(gate-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(gate-impl PUBLIC GATE_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (GATE_BUILD_TOOLS)
        add_executable(gate-host-smoke test/host_smoke.cpp)
        target_link_libraries(gate-host-smoke PRIVATE clap)
        if (GATE_RTSAN)
            target_link_options(gate-host-smoke PRIVATE -fsanitize=realtime)
        endif()
        target_compile_definitions(gate-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.gate"
            HOST_SMOKE_PLAIN_PARAM="Output"
            HOST_SMOKE_LATENCY_PARAM="Lookahead")
        gate_target_defaults(gate-host-smoke)
        add_test(NAME host-smoke
                 COMMAND gate-host-smoke
                         ${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.clap)
        add_test(NAME host-smoke-mono
                 COMMAND gate-host-smoke
                         ${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.clap --mono)
    endif()

    option(GATE_VST3 "Build VST3 format" OFF)
    set(GATE_FORMATS CLAP)
    if (GATE_VST3)
        list(APPEND GATE_FORMATS VST3)
    endif()
    if (APPLE)
        list(APPEND GATE_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME gate-plugin
        IMPL_TARGET gate-impl
        OUTPUT_NAME "${GATE_DISPLAY_NAME}"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/gate_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.gate"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${GATE_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${GATE_ASSET_DIR}
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QGat"
        AUV2_INSTRUMENT_TYPE "aufx")
    if (TARGET gate-plugin_clap)
        gate_target_defaults(gate-plugin_clap)
    endif()
    if (GATE_RTSAN AND TARGET gate-plugin_clap)
        target_link_options(gate-plugin_clap PRIVATE -fsanitize=realtime)
    elseif (SUPPLE_SANITIZE STREQUAL "thread" AND TARGET gate-plugin_clap)
        target_link_options(gate-plugin_clap PRIVATE -fsanitize=thread)
    endif()

    # Linux modules export their entry points and nothing else. Every other
    # symbol is visible to each plugin in the host process, and the GNU unique
    # ones among them (fmt's locale facet id, the VST3 SDK's UpdateHandler
    # singleton) bind across modules whatever RTLD_LOCAL says: the second
    # plugin runs on the first one's copies, and glibc pins the first against
    # dlclose. -fvisibility=hidden cannot do this job -- libstdc++ and pugl
    # mark their own symbols default -- so the linker gets an explicit list.
    # macOS binds symbols per image (two-level namespace) and Windows exports
    # only what is dllexport'd, so neither needs it.
    if (UNIX AND NOT APPLE)
        foreach(format clap vst3)
            if (TARGET gate-plugin_${format})
                set(gate_exports ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports-${format}.map)
                target_link_options(gate-plugin_${format} PRIVATE
                    "LINKER:--version-script=${gate_exports}")
                set_property(TARGET gate-plugin_${format} APPEND PROPERTY
                    LINK_DEPENDS ${gate_exports})
            endif()
        endforeach()

        if (GATE_BUILD_TOOLS AND TARGET gate-plugin_clap)
            add_test(NAME exports-clap
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:gate-plugin_clap>
                    "-DEXPECTED=clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
        endif()

        if (GATE_BUILD_TOOLS AND TARGET gate-plugin_vst3)
            add_test(NAME wrapper-self-handle COMMAND ${GIT_EXECUTABLE}
                -C ${clap-wrapper_SOURCE_DIR} apply --reverse --check
                ${gate_wrapper_self_handle_patch})
            add_test(NAME exports-vst3
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:gate-plugin_vst3>
                    "-DEXPECTED=GetPluginFactory,ModuleEntry,ModuleExit,clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
            add_executable(gate-vst3-unload-probe test/vst3_unload_probe.cpp)
            target_link_libraries(gate-vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
            gate_target_defaults(gate-vst3-unload-probe)
            add_test(NAME vst3-unload
                COMMAND gate-vst3-unload-probe $<TARGET_FILE:gate-plugin_vst3>)
        endif()
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
    set(gate_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(gate_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(gate-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "gate-plugin_clap:${GATE_DISPLAY_NAME}.clap" "gate-plugin_vst3:${GATE_DISPLAY_NAME}.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(gate-notices ${target})
                add_custom_command(TARGET gate-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${GATE_ASSET_DIR}/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_notices}"
                        "${GATE_ASSET_DIR}/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_license}"
                        "${GATE_ASSET_DIR}/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET gate-notices POST_BUILD
            # The plugins create that directory when they link, but a clean build
            # can reach this copy first, so create it here as well.
            COMMAND ${CMAKE_COMMAND} -E make_directory "${GATE_ASSET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_notices}"
                "${GATE_ASSET_DIR}/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_license}"
                "${GATE_ASSET_DIR}/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET gate-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(gate-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/gate-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_notices}"
                "${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${gate_license}"
                "${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component"
            DEPENDS gate-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${GATE_DISPLAY_NAME}.component"
            VERBATIM)
        if (GATE_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${GATE_ASSET_DIR}/${GATE_DISPLAY_NAME}.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(GATE_INSTALL_SPECS
            "${GATE_DISPLAY_NAME}.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:gate-plugin_clap:dir"
            "${GATE_DISPLAY_NAME}.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:gate-plugin_vst3:dir"
            "${GATE_DISPLAY_NAME}.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:gate-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(GATE_INSTALL_SPECS
            "${GATE_DISPLAY_NAME}.clap:$ENV{HOME}/.clap:gate-plugin_clap:file"
            "${GATE_DISPLAY_NAME}.vst3:$ENV{HOME}/.vst3:gate-plugin_vst3:dir")
    endif()

    if (GATE_INSTALL_SPECS)
        add_custom_target(gate-install COMMENT "Installing every ${GATE_DISPLAY_NAME} format")
        if (TARGET gate-prepare-auv2)
            add_dependencies(gate-install gate-prepare-auv2)
        endif()
        foreach(spec ${GATE_INSTALL_SPECS})
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
                add_dependencies(gate-install ${target})
                add_custom_command(TARGET gate-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${GATE_ASSET_DIR}/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET gate-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
