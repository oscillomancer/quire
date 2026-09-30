cmake_minimum_required(VERSION 3.21)

# This must be set before project(): project() creates the cache entry, and a
# later non-FORCE assignment leaves it empty. macOS 11 is the first release that
# can run the arm64 slice produced by current Apple Silicon builds.
if (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

if (APPLE)
    project(delay VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
else()
    project(delay VERSION 0.1.0 LANGUAGES C CXX)
endif()

# A bare `cmake -S . -B build` sets no build type on single-config generators,
# which silently produces an unoptimised plugin. The presets always name one;
# this is for everybody who does not use them.
get_property(_delay_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if (NOT _delay_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "No CMAKE_BUILD_TYPE given; defaulting to Release")
endif()



set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(DELAY_BUILD_PLUGIN "Build CLAP / VST3 / AUv2 formats" ON)
option(DELAY_BUILD_TOOLS "Build tests and integration probes" ON)
option(DELAY_BUILD_GUI "Build the Pugl + NanoVG editor" ON)
option(DELAY_STRICT_WARNINGS "Enable warnings on delay-owned code" ON)
option(DELAY_SANITIZERS "Enable AddressSanitizer and UBSan on delay-owned code" OFF)

function(delay_target_defaults target)
    if (DELAY_STRICT_WARNINGS)
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
    if (DELAY_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_library(delay-dsp INTERFACE)
target_include_directories(delay-dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dsp/include)

if (DELAY_BUILD_TOOLS)
    enable_testing()
    add_executable(delay-cli tools/delay_cli.cpp)
    target_link_libraries(delay-cli PRIVATE delay-dsp)
    delay_target_defaults(delay-cli)
    add_executable(delay-test test/test_core.cpp)
    target_link_libraries(delay-test PRIVATE delay-dsp)
    delay_target_defaults(delay-test)
    add_test(NAME core COMMAND delay-test)
endif()

function(delay_add_gui_preview)
    if (TARGET delay-gui-gl)
        set(preview_gui delay-gui-gl)
    else()
        set(preview_gui delay-gui)
    endif()
    add_executable(delay-gui-preview tools/gui_preview.cpp)
    target_link_libraries(delay-gui-preview PRIVATE ${preview_gui})
    target_include_directories(delay-gui-preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
endfunction()

if (DELAY_BUILD_PLUGIN)
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
    set(delay_wrapper_vst3_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-all-points.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${delay_wrapper_vst3_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_patch_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_patch_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${delay_wrapper_vst3_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${delay_wrapper_vst3_patch}
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
    set(delay_wrapper_vst3_patch_timer ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-timer-floor.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${delay_wrapper_vst3_patch_timer}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_timer_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_timer_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${delay_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${delay_wrapper_vst3_patch_timer}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_timer_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_timer_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 timer floor patch no longer applies")
        endif()
    endif()

    # On Linux the wrapper reopens its own module to find its statically linked
    # CLAP entry, and keeps that handle. The extra reference pins the module
    # against the host's dlclose, and when the process exits ~Library closes it
    # from inside the module's own static destructors, unmapping code that is
    # still running. Separately it looks the entry up with dlsym(_handle) while
    # _handle is still null, which searches the whole process and can return
    # another plugin's clap_entry. glibc's NODELETE on the exported GNU unique
    # symbols used to hide the first fault; the export lists below remove that
    # cover, so the fault has to be fixed at its source.
    set(delay_wrapper_self_handle_patch ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-wrapper-vst3-self-handle.patch)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} apply --check ${delay_wrapper_self_handle_patch}
        WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
        RESULT_VARIABLE wrapper_self_handle_applicable
        OUTPUT_QUIET ERROR_QUIET)
    if (wrapper_self_handle_applicable EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply ${delay_wrapper_self_handle_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${delay_wrapper_self_handle_patch}
            WORKING_DIRECTORY ${clap-wrapper_SOURCE_DIR}
            RESULT_VARIABLE wrapper_self_handle_present
            OUTPUT_QUIET ERROR_QUIET)
        if (NOT wrapper_self_handle_present EQUAL 0)
            message(FATAL_ERROR "clap-wrapper VST3 self-handle patch no longer applies")
        endif()
    endif()

    if (DELAY_BUILD_TOOLS)
        add_test(NAME wrapper-automation
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/detail/vst3/process.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_automation.cmake)
        add_test(NAME wrapper-timer
            COMMAND ${CMAKE_COMMAND}
                -DWRAPPER_FILE=${clap-wrapper_SOURCE_DIR}/src/wrapasvst3.cpp
                -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_wrapper_timer.cmake)
    endif()

    add_library(delay-impl STATIC plugin/src/delay_clap.cpp)
    target_link_libraries(delay-impl PUBLIC clap clap-wrapper-extensions delay-dsp)
    delay_target_defaults(delay-impl)

    if (DELAY_BUILD_GUI)
        add_subdirectory(plugin/gui)
        target_link_libraries(delay-impl PUBLIC delay-gui)
        target_compile_definitions(delay-impl PUBLIC DELAY_WITH_GUI=1)
        if (DELAY_BUILD_TOOLS)
            delay_add_gui_preview()
            add_executable(delay-editor-test test/editor_input.cpp)
            target_link_libraries(delay-editor-test PRIVATE
                $<IF:$<TARGET_EXISTS:delay-gui-gl>,delay-gui-gl,delay-gui>)
            target_include_directories(delay-editor-test PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            add_test(NAME editor-input COMMAND delay-editor-test)

            if (UNIX AND NOT APPLE)
                find_package(X11 REQUIRED)
                find_package(Threads REQUIRED)
                add_executable(delay-linux-host-probe tools/linux_host_probe.cpp)
                target_link_libraries(delay-linux-host-probe PRIVATE clap X11::X11 ${CMAKE_DL_LIBS} Threads::Threads)
            endif()
            add_executable(delay-clap-probe tools/clap_host_probe.cpp)
            target_link_libraries(delay-clap-probe PRIVATE clap)
            target_include_directories(delay-clap-probe PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/plugin/src)
            if (APPLE)
                set_source_files_properties(tools/clap_host_probe.cpp PROPERTIES LANGUAGE OBJCXX)
                target_compile_options(delay-clap-probe PRIVATE -fno-objc-arc)
                target_link_libraries(delay-clap-probe PRIVATE "-framework Cocoa")
            endif()
        endif()
    else()
        target_compile_definitions(delay-impl PUBLIC DELAY_WITH_GUI=0)
    endif()

    # A tiny host that dlopens the built .clap and drives it the way a DAW
    # would. The CLAP is what clap-wrapper turns into the VST3 and the AU, so
    # exercising it covers the shared code path in all three formats.
    if (DELAY_BUILD_TOOLS)
        add_executable(delay-host-smoke test/host_smoke.cpp)
        target_link_libraries(delay-host-smoke PRIVATE clap)
        target_compile_definitions(delay-host-smoke PRIVATE
            HOST_SMOKE_PLUGIN_ID="com.suppleaudio.delay"
            HOST_SMOKE_PLAIN_PARAM="Input Gain")
        delay_target_defaults(delay-host-smoke)
        add_test(NAME host-smoke
                 COMMAND delay-host-smoke
                         ${CMAKE_BINARY_DIR}/delay_assets/Delay.clap)
        add_test(NAME host-smoke-mono
                 COMMAND delay-host-smoke
                         ${CMAKE_BINARY_DIR}/delay_assets/Delay.clap --mono)
    endif()

    set(DELAY_FORMATS CLAP VST3)
    if (APPLE)
        list(APPEND DELAY_FORMATS AUV2)
    endif()

    # CUSTOMIZE BEFORE SHIPPING: every product needs permanent unique IDs and
    # AU four-character codes (see the checklist in AGENTS.md).
    make_clapfirst_plugins(
        TARGET_NAME delay-plugin
        IMPL_TARGET delay-impl
        OUTPUT_NAME "Delay"
        ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/plugin/src/delay_entry.cpp"
        BUNDLE_IDENTIFIER "com.suppleaudio.delay"
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${DELAY_FORMATS}
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/delay_assets
        AUV2_MANUFACTURER_NAME "Supple Audio"
        AUV2_MANUFACTURER_CODE "Supl"
        AUV2_SUBTYPE_CODE "QDly"
        AUV2_INSTRUMENT_TYPE "aufx")

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
            if (TARGET delay-plugin_${format})
                set(delay_exports ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports-${format}.map)
                target_link_options(delay-plugin_${format} PRIVATE
                    "LINKER:--version-script=${delay_exports}")
                set_property(TARGET delay-plugin_${format} APPEND PROPERTY
                    LINK_DEPENDS ${delay_exports})
            endif()
        endforeach()

        if (DELAY_BUILD_TOOLS AND TARGET delay-plugin_vst3)
            add_test(NAME wrapper-self-handle COMMAND ${GIT_EXECUTABLE}
                -C ${clap-wrapper_SOURCE_DIR} apply --reverse --check
                ${delay_wrapper_self_handle_patch})
            add_test(NAME exports-vst3
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:delay-plugin_vst3>
                    "-DEXPECTED=GetPluginFactory,ModuleEntry,ModuleExit,clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
            add_test(NAME exports-clap
                COMMAND ${CMAKE_COMMAND}
                    -DMODULE=$<TARGET_FILE:delay-plugin_clap>
                    "-DEXPECTED=clap_entry"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_plugin_exports.cmake)
            add_executable(delay-vst3-unload-probe test/vst3_unload_probe.cpp)
            target_link_libraries(delay-vst3-unload-probe PRIVATE ${CMAKE_DL_LIBS})
            delay_target_defaults(delay-vst3-unload-probe)
            add_test(NAME vst3-unload
                COMMAND delay-vst3-unload-probe $<TARGET_FILE:delay-plugin_vst3>)
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
    set(delay_notices "${CMAKE_CURRENT_SOURCE_DIR}/NOTICES.txt")
    set(delay_license "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

    add_custom_target(delay-notices ALL
        COMMENT "Staging the license and third-party notices with the built plugins")

    if (APPLE)
        # All three macOS formats are bundles, so the notices go where a bundle
        # is supposed to keep them. The AU is absent here on purpose: its copy
        # has to precede the code signature, so it lives in the prepare step
        # below instead.
        foreach(spec "delay-plugin_clap:Delay.clap" "delay-plugin_vst3:Delay.vst3")
            string(REPLACE ":" ";" parts ${spec})
            list(GET parts 0 target)
            list(GET parts 1 bundle)
            if (TARGET ${target})
                add_dependencies(delay-notices ${target})
                add_custom_command(TARGET delay-notices POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${CMAKE_BINARY_DIR}/delay_assets/${bundle}/Contents/Resources"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_notices}"
                        "${CMAKE_BINARY_DIR}/delay_assets/${bundle}/Contents/Resources/NOTICES.txt"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_license}"
                        "${CMAKE_BINARY_DIR}/delay_assets/${bundle}/Contents/Resources/LICENSE.txt"
                    VERBATIM)
            endif()
        endforeach()
    else()
        # A CLAP on Linux or Windows is a single shared library with no bundle
        # to put anything in, so the file lands beside the built artifacts and
        # it is the release archive that has to carry it.
        add_custom_command(TARGET delay-notices POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_notices}"
                "${CMAKE_BINARY_DIR}/delay_assets/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_license}"
                "${CMAKE_BINARY_DIR}/delay_assets/LICENSE.txt"
            VERBATIM)
    endif()

    if (APPLE AND TARGET delay-plugin_auv2)
        # CMake regenerates the bundle's generic Info.plist at configure time,
        # while clap-wrapper writes the real AudioComponents plist from its
        # build helper. If the AU binary is already up to date, its post-build
        # copy does not rerun and an install can otherwise copy an unregistered
        # component. Prepare it unconditionally before aggregate builds/install.
        add_custom_target(delay-prepare-auv2 ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/delay-plugin_auv2-build-helper-output/auv2_Info.plist"
                "${CMAKE_BINARY_DIR}/delay_assets/Delay.component/Contents/Info.plist"
            # Before the signature, not after: adding a file to a signed bundle
            # invalidates it, and the auv2-bundle test checks the signature.
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/delay_assets/Delay.component/Contents/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_notices}"
                "${CMAKE_BINARY_DIR}/delay_assets/Delay.component/Contents/Resources/NOTICES.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${delay_license}"
                "${CMAKE_BINARY_DIR}/delay_assets/Delay.component/Contents/Resources/LICENSE.txt"
            COMMAND /usr/bin/codesign --force --deep --sign - --timestamp=none
                "${CMAKE_BINARY_DIR}/delay_assets/Delay.component"
            DEPENDS delay-plugin_auv2
            COMMENT "Preparing and ad-hoc signing Delay.component"
            VERBATIM)
        if (DELAY_BUILD_TOOLS)
            add_test(NAME auv2-bundle
                COMMAND ${CMAKE_COMMAND}
                    -DBUNDLE=${CMAKE_BINARY_DIR}/delay_assets/Delay.component
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_auv2_bundle.cmake)
        endif()
    endif()

    if (APPLE)
        set(DELAY_INSTALL_SPECS
            "Delay.clap:$ENV{HOME}/Library/Audio/Plug-Ins/CLAP:delay-plugin_clap:dir"
            "Delay.vst3:$ENV{HOME}/Library/Audio/Plug-Ins/VST3:delay-plugin_vst3:dir"
            "Delay.component:$ENV{HOME}/Library/Audio/Plug-Ins/Components:delay-plugin_auv2:dir")
    elseif (UNIX)
        # On Linux a CLAP is a bare shared object, not a bundle directory --
        # only the VST3 is a folder here -- so the two are copied differently.
        set(DELAY_INSTALL_SPECS
            "Delay.clap:$ENV{HOME}/.clap:delay-plugin_clap:file"
            "Delay.vst3:$ENV{HOME}/.vst3:delay-plugin_vst3:dir")
    endif()

    if (DELAY_INSTALL_SPECS)
        add_custom_target(delay-install COMMENT "Installing every Delay format")
        if (TARGET delay-prepare-auv2)
            add_dependencies(delay-install delay-prepare-auv2)
        endif()
        foreach(spec ${DELAY_INSTALL_SPECS})
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
                add_dependencies(delay-install ${target})
                add_custom_command(TARGET delay-install POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E rm -rf "${destination}/${bundle}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination}"
                    COMMAND ${CMAKE_COMMAND} -E ${copyCmd}
                        "${CMAKE_BINARY_DIR}/delay_assets/${bundle}"
                        "${destination}/${bundle}"
                    VERBATIM)
            endif()
        endforeach()
        if (APPLE)
            add_custom_command(TARGET delay-install POST_BUILD
                COMMAND /bin/sh -c "killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true"
                VERBATIM)
        endif()
    endif()
endif()
