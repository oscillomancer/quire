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
    project(comp VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(comp VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_comp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _comp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(COMP_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(COMP_BUILD_TOOLS "Build tests and integration probes" ON)
option(COMP_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(COMP_STRICT_WARNINGS "Enable warnings on comp-owned code" ON)
option(COMP_SANITIZERS "Enable AddressSanitizer and UBSan on comp-owned code" OFF)
option(COMP_RTSAN "Instrument the SDK-free DSP test with RealtimeSanitizer" OFF)
# Where the built formats are written (the template's layout); the Quire
# monorepo build passes its shared plugins/clap folder instead.
set(COMP_ASSET_DIR "${CMAKE_BINARY_DIR}/comp_assets" CACHE PATH
    "Directory the plugin formats are written to")

function(comp_target_defaults target)
    if (COMP_STRICT_WARNINGS)
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
    if (COMP_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
    target_compile_options(${target} PRIVATE -ffp-contract=off)
    if (COMP_RTSAN)
        target_compile_options(${target} PRIVATE -fsanitize=realtime)
        target_link_options(${target} PRIVATE -fsanitize=realtime)
    endif()
    if (SUPPLE_SANITIZE STREQUAL "thread")
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif()
endfunction()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Identity.cmake)
include(${SUPPLE_ROOT}/plugins/common/cmake/LinkMonorepo.cmake)
supple_plugin_add_monorepo_libraries()
# Engine targets are outside the helper function: retain sanitizer flags here.
include(${SUPPLE_ROOT}/cmake/Defaults.cmake)

add_subdirectory(engine "${CMAKE_BINARY_DIR}/supple/comp-engine")
add_library(comp-dsp INTERFACE)
target_link_libraries(comp-dsp INTERFACE supple::comp-engine)

if (COMP_BUILD_TOOLS)
    enable_testing()
    add_executable(comp-cli tools/comp_cli.cpp)
    target_link_libraries(comp-cli PRIVATE comp-dsp)
    comp_target_defaults(comp-cli)
    add_executable(comp-test test/test_core.cpp)
    target_link_libraries(comp-test PRIVATE comp-dsp)
    comp_target_defaults(comp-test)
    if (COMP_RTSAN)
        target_compile_options(comp-test PRIVATE -fsanitize=realtime)
        target_link_options(comp-test PRIVATE -fsanitize=realtime)
    endif()
    add_test(NAME core COMMAND comp-test)
    if (UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        add_executable(comp-semantic-rt test/semantic_rt.cpp)
        target_link_libraries(comp-semantic-rt PRIVATE
            supple::plugin-support Threads::Threads ${CMAKE_DL_LIBS})
        comp_target_defaults(comp-semantic-rt)
        if (COMP_RTSAN)
            target_compile_options(comp-semantic-rt PRIVATE -fsanitize=realtime)
            target_link_options(comp-semantic-rt PRIVATE -fsanitize=realtime)
        endif()
        add_test(NAME semantic-rt COMMAND comp-semantic-rt
            ${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.clap)
    endif()

    if (SUPPLE_SEMANTIC_CONFORMANCE)
        add_test(NAME semantic-conformance COMMAND ${SUPPLE_SEMANTIC_CONFORMANCE}
            ${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.clap com.suppleaudio.comp)
    endif()
endif()

function(comp_add_gui_preview)
    if (TARGET comp-gui-gl)
        set(preview_gui comp-gui-gl)
    else()
        set(preview_gui comp-gui)
    endif()
    add_executable(comp-gui-preview tools/gui_preview.cpp)
    target_link_libraries(comp-gui-preview PRIVATE ${preview_gui})
    target_include_directories(comp-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (COMP_BUILD_PLUGIN)
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
    set(comp_wrapper_self_handle_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-self-handle.patch)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check
        ${comp_wrapper_self_handle_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR} RESULT_VARIABLE self_handle_present)
    if (NOT points_present EQUAL 0 OR NOT timer_present EQUAL 0 OR NOT self_handle_present EQUAL 0)
        message(FATAL_ERROR "Vendored clap-wrapper lacks template VST3 patches")
    endif()

    if (COMP_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(comp-impl STATIC plugin/src/comp_clap.cpp)
    target_link_libraries(comp-impl PUBLIC clap clap-wrapper-extensions comp-dsp supple::plugin-support)
    if (COMP_RTSAN)
        target_compile_options(comp-impl PRIVATE -fsanitize=realtime)
    endif()
    comp_target_defaults(comp-impl)

    if (COMP_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(comp-impl PUBLIC comp-gui)
        target_compile_definitions(comp-impl PUBLIC COMP_WITH_GUI=1)
        if (COMP_BUILD_TOOLS)
            comp_add_gui_preview()
            add_executable(comp-editor-test test/editor_input.cpp)
            target_link_libraries(comp-editor-test PRIVATE supple::plugin-support
                $<IF:$<TARGET_EXISTS:comp-gui-gl>,comp-gui-gl,comp-gui>)
            target_include_directories(comp-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND comp-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(comp-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(comp-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(comp-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(comp-clap-probe PRIVATE clap comp-dsp)
            target_include_directories(comp-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(comp-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(comp-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(comp-impl PUBLIC COMP_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (COMP_BUILD_TOOLS)
        add_executable(comp-host-smoke test/host_smoke.cpp)
        target_link_libraries(comp-host-smoke PRIVATE clap)
        if (COMP_RTSAN)
            target_link_options(comp-host-smoke PRIVATE -fsanitize=realtime)
        endif()
        target_compile_definitions(comp-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.comp"
            HOST_SMOKE_PLAIN_PARAM="Output"
            HOST_SMOKE_LATENCY_PARAM="Lookahead")
        comp_target_defaults(comp-host-smoke)
        add_test(NAME host-smoke
                 COMMAND comp-host-smoke
                         ${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.clap)
        add_test(NAME host-smoke-mono
                 COMMAND comp-host-smoke
                         ${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.clap --mono)
    endif()

    option(COMP_VST3 "Build VST3 format" OFF)
    set(COMP_FORMATS CLAP)
    if (COMP_VST3)
        list(APPEND COMP_FORMATS VST3)
    endif()
    if (APPLE)
        list(APPEND COMP_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME comp-plugin
        IMPL_TARGET comp-impl
        OUTPUT_NAME "${COMP_DISPLAY_NAME}"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/comp_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.comp"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${COMP_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${COMP_ASSET_DIR}
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QCmp"
        AUV2_INSTRUMENT_TYPE "aufx")
    if (COMP_RTSAN AND TARGET comp-plugin_clap)
        target_link_options(comp-plugin_clap PRIVATE -fsanitize=realtime)
    elseif (SUPPLE_SANITIZE STREQUAL "thread" AND TARGET comp-plugin_clap)
        target_link_options(comp-plugin_clap PRIVATE -fsanitize=thread)
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
            if (TARGET comp-plugin_${format})
                set(comp_exports ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports-${format}.map)
                target_link_options(comp-plugin_${format} PRIVATE
                    "LINKER:--version-script=${comp_exports}")
                set_property(TARGET comp-plugin_${format} APPEND PROPERTY
                    LINK_DEPENDS ${comp_exports})
            endif()
        endforeach()

        if (COMP_BUILD_TOOLS AND TARGET comp-plugin_clap)
            add_test(NAME exports-clap
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:comp-plugin_clap>
                    "-DEXPECTED=clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
        endif()

        if (COMP_BUILD_TOOLS AND TARGET comp-plugin_vst3)
            add_test(NAME wrapper-self-handle COMMAND ${GIT_EXECUTABLE}
                -C ${clap-wrapper_SOURCE_DIR} apply --reverse --check
                ${comp_wrapper_self_handle_patch})
            add_test(NAME exports-vst3
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:comp-plugin_vst3>
                    "-DEXPECTED=GetPluginFactory,ModuleEntry,ModuleExit,clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
            add_executable(comp-vst3-unload-probe test/vst3_unload_probe.cpp)
            target_link_libraries(comp-vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
            comp_target_defaults(comp-vst3-unload-probe)
            add_test(NAME vst3-unload
                COMMAND comp-vst3-unload-probe $<TARGET_FILE:comp-plugin_vst3>)
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
    set(comp_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(comp_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(comp-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "comp-plugin_clap:${COMP_DISPLAY_NAME}.clap" "comp-plugin_vst3:${COMP_DISPLAY_NAME}.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(comp-notices ${target})
                add_custom_command(TARGET comp-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${COMP_ASSET_DIR}/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_notices}"
                        "${COMP_ASSET_DIR}/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_license}"
                        "${COMP_ASSET_DIR}/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET comp-notices POST_BUILD
            # The plugins create that directory when they link, but a clean build
            # can reach this copy first, so create it here as well.
            COMMAND ${CMAKE_COMMAND} -E make_directory "${COMP_ASSET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_notices}"
                "${COMP_ASSET_DIR}/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_license}"
                "${COMP_ASSET_DIR}/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET comp-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(comp-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/comp-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_notices}"
                "${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${comp_license}"
                "${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component"
            DEPENDS comp-plugin_auv2
            COMMENT "Preparing and ad-hoc signing ${COMP_DISPLAY_NAME}.component"
            VERBATIM)
        if (COMP_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${COMP_ASSET_DIR}/${COMP_DISPLAY_NAME}.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(COMP_INSTALL_SPECS
            "${COMP_DISPLAY_NAME}.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:comp-plugin_clap:dir"
            "${COMP_DISPLAY_NAME}.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:comp-plugin_vst3:dir"
            "${COMP_DISPLAY_NAME}.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:comp-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(COMP_INSTALL_SPECS
            "${COMP_DISPLAY_NAME}.clap:$ENV{HOME}/.clap:comp-plugin_clap:file"
            "${COMP_DISPLAY_NAME}.vst3:$ENV{HOME}/.vst3:comp-plugin_vst3:dir")
    endif()

    if (COMP_INSTALL_SPECS)
        add_custom_target(comp-install COMMENT "Installing every Comp format")
        if (TARGET comp-prepare-auv2)
            add_dependencies(comp-install comp-prepare-auv2)
        endif()
        foreach(spec ${COMP_INSTALL_SPECS})
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
                add_dependencies(comp-install ${target})
                add_custom_command(TARGET comp-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${COMP_ASSET_DIR}/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET comp-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
