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
    project(drive VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(drive VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_drive_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _drive_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(DRIVE_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(DRIVE_BUILD_TOOLS "Build tests and integration probes" ON)
option(DRIVE_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(DRIVE_STRICT_WARNINGS "Enable warnings on drive-owned code" ON)
option(DRIVE_SANITIZERS "Enable AddressSanitizer and UBSan on drive-owned code" OFF)
option(DRIVE_RTSAN "Instrument the SDK-free DSP test with RealtimeSanitizer" OFF)
# Where the built formats are written (the template's layout); the Quire
# monorepo build passes its shared plugins/clap folder instead.
set(DRIVE_ASSET_DIR "${CMAKE_BINARY_DIR}/drive_assets" CACHE PATH
    "Directory the plugin formats are written to")

function(drive_target_defaults target)
    if (DRIVE_STRICT_WARNINGS)
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
    if (DRIVE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    if (SUPPLE_SANITIZE STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Identity.cmake)
include(${SUPPLE_ROOT}/plugins/common/cmake/LinkMonorepo.cmake)
supple_plugin_add_monorepo_libraries()

set(SUPPLE_BUILD_TESTS OFF)
add_subdirectory(engine ${CMAKE_BINARY_DIR}/supple/drive-engine)
add_library(drive-dsp INTERFACE)
target_link_libraries(drive-dsp INTERFACE supple::drive-engine)
if(DRIVE_BUILD_TOOLS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(drive-semantic-rt test/semantic_rt.cpp)
    target_link_libraries(drive-semantic-rt PRIVATE supple::plugin-support Threads::Threads ${CMAKE_DL_LIBS})
    drive_target_defaults(drive-semantic-rt)
    if(DRIVE_RTSAN)
        target_compile_options(drive-semantic-rt PRIVATE -fsanitize=realtime)
        target_link_options(drive-semantic-rt PRIVATE -fsanitize=realtime)
    endif()
    add_test(NAME semantic-rt COMMAND drive-semantic-rt ${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.clap)
    if(SUPPLE_SEMANTIC_CONFORMANCE)
        add_test(NAME semantic-conformance COMMAND ${SUPPLE_SEMANTIC_CONFORMANCE}
            ${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.clap com.suppleaudio.drive)
    endif()
endif()

function(drive_add_gui_preview)
    if (TARGET drive-gui-gl)
        set(preview_gui drive-gui-gl)
    else()
        set(preview_gui drive-gui)
    endif()
    add_executable(drive-gui-preview tools/gui_preview.cpp)
    target_link_libraries(drive-gui-preview PRIVATE ${preview_gui})
    target_include_directories(drive-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (DRIVE_BUILD_PLUGIN)
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
    set(drive_wrapper_self_handle_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-self-handle.patch)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${drive_wrapper_self_handle_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE self_handle_present)
    if (NOT points_present EQUAL 0 OR NOT timer_present EQUAL 0 OR NOT self_handle_present EQUAL 0)
        message(FATAL_ERROR "Vendored clap-wrapper lacks template VST3 patches")
    endif()

    if (DRIVE_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(drive-impl STATIC plugin/src/drive_clap.cpp)
    target_link_libraries(drive-impl PUBLIC clap clap-wrapper-extensions drive-dsp supple::plugin-support)
    if (DRIVE_RTSAN)
        target_compile_options(drive-impl PRIVATE -fsanitize=realtime)
    endif()
    drive_target_defaults(drive-impl)

    if (DRIVE_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(drive-impl PUBLIC drive-gui)
        target_compile_definitions(drive-impl PUBLIC DRIVE_WITH_GUI=1)
        if (DRIVE_BUILD_TOOLS)
            drive_add_gui_preview()
            add_executable(drive-editor-test test/editor_input.cpp)
            target_link_libraries(drive-editor-test PRIVATE supple::plugin-support
                $<IF:$<TARGET_EXISTS:drive-gui-gl>,drive-gui-gl,drive-gui>)
            target_include_directories(drive-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND drive-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(drive-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(drive-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(drive-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(drive-clap-probe PRIVATE clap drive-dsp)
            target_include_directories(drive-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(drive-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(drive-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(drive-impl PUBLIC DRIVE_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (DRIVE_BUILD_TOOLS)
        add_executable(drive-host-smoke test/host_smoke.cpp)
        target_link_libraries(drive-host-smoke PRIVATE clap)
        if (DRIVE_RTSAN)
            target_link_options(drive-host-smoke PRIVATE -fsanitize=realtime)
        endif()
        target_compile_definitions(drive-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.drive"
            HOST_SMOKE_PLAIN_PARAM="Drive"
            HOST_SMOKE_LATENCY_PARAM="Oversampling")
        drive_target_defaults(drive-host-smoke)
        add_test(NAME host-smoke
                 COMMAND drive-host-smoke
                         ${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.clap)
        add_test(NAME host-smoke-mono
                 COMMAND drive-host-smoke
                         ${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.clap --mono)
    endif()

    option(DRIVE_VST3 "Build VST3 format" OFF)
    set(DRIVE_FORMATS CLAP)
    if (DRIVE_VST3)
        list(APPEND DRIVE_FORMATS VST3)
    endif()
    if (APPLE)
        list(APPEND DRIVE_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME drive-plugin
        IMPL_TARGET drive-impl
        OUTPUT_NAME "${DRIVE_DISPLAY_NAME}"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/drive_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.drive"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${DRIVE_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${DRIVE_ASSET_DIR}
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QDrv"
        AUV2_INSTRUMENT_TYPE "aufx")
    if (DRIVE_RTSAN AND TARGET drive-plugin_clap)
        target_link_options(drive-plugin_clap PRIVATE -fsanitize=realtime)
    elseif (SUPPLE_SANITIZE STREQUAL "thread" AND TARGET drive-plugin_clap)
        target_link_options(drive-plugin_clap PRIVATE -fsanitize=thread)
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
            if (TARGET drive-plugin_${format})
                set(drive_exports ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports-${format}.map)
                target_link_options(drive-plugin_${format} PRIVATE
                    "LINKER:--version-script=${drive_exports}")
                set_property(TARGET drive-plugin_${format} APPEND PROPERTY
                    LINK_DEPENDS ${drive_exports})
            endif()
        endforeach()

        if (DRIVE_BUILD_TOOLS AND TARGET drive-plugin_clap)
            add_test(NAME exports-clap
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:drive-plugin_clap>
                    "-DEXPECTED=clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
        endif()

        if (DRIVE_BUILD_TOOLS AND TARGET drive-plugin_vst3)
            add_test(NAME wrapper-self-handle COMMAND ${GIT_EXECUTABLE}
                -C ${clap-wrapper_SOURCE_DIR} apply --reverse --check
                ${drive_wrapper_self_handle_patch})
            add_test(NAME exports-vst3
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:drive-plugin_vst3>
                    "-DEXPECTED=GetPluginFactory,ModuleEntry,ModuleExit,clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
            add_executable(drive-vst3-unload-probe test/vst3_unload_probe.cpp)
            target_link_libraries(drive-vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
            drive_target_defaults(drive-vst3-unload-probe)
            add_test(NAME vst3-unload
                COMMAND drive-vst3-unload-probe $<TARGET_FILE:drive-plugin_vst3>)
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
    set(drive_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(drive_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(drive-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "drive-plugin_clap:${DRIVE_DISPLAY_NAME}.clap" "drive-plugin_vst3:${DRIVE_DISPLAY_NAME}.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(drive-notices ${target})
                add_custom_command(TARGET drive-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${DRIVE_ASSET_DIR}/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_notices}"
                        "${DRIVE_ASSET_DIR}/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_license}"
                        "${DRIVE_ASSET_DIR}/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET drive-notices POST_BUILD
            # The plugins create that directory when they link, but a clean build
            # can reach this copy first, so create it here as well.
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVE_ASSET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_notices}"
                "${DRIVE_ASSET_DIR}/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_license}"
                "${DRIVE_ASSET_DIR}/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET drive-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(drive-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/drive-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_notices}"
                "${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${drive_license}"
                "${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component"
            DEPENDS drive-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${DRIVE_DISPLAY_NAME}.component"
            VERBATIM)
        if (DRIVE_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${DRIVE_ASSET_DIR}/${DRIVE_DISPLAY_NAME}.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(DRIVE_INSTALL_SPECS
            "${DRIVE_DISPLAY_NAME}.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:drive-plugin_clap:dir"
            "${DRIVE_DISPLAY_NAME}.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:drive-plugin_vst3:dir"
            "${DRIVE_DISPLAY_NAME}.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:drive-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(DRIVE_INSTALL_SPECS
            "${DRIVE_DISPLAY_NAME}.clap:$ENV{HOME}/.clap:drive-plugin_clap:file"
            "${DRIVE_DISPLAY_NAME}.vst3:$ENV{HOME}/.vst3:drive-plugin_vst3:dir")
    endif()

    if (DRIVE_INSTALL_SPECS)
        add_custom_target(drive-install COMMENT "Installing every Drive format")
        if (TARGET drive-prepare-auv2)
            add_dependencies(drive-install drive-prepare-auv2)
        endif()
        foreach(spec ${DRIVE_INSTALL_SPECS})
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
                add_dependencies(drive-install ${target})
                add_custom_command(TARGET drive-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${DRIVE_ASSET_DIR}/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET drive-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
