# Third-party sources, fetched at configure time and pinned to exact commits
# (cmake/Pins.cmake): a branch name would let two checkouts of the same
# source build different plugins.
#
# Offline: tools/vendor.cmake copies every pinned source into a folder
# (vendor/ at the root by default), and a configure that finds one there --
# or is given -DSUPPLE_VENDOR_DIR=<folder> -- builds from it with no network
# at all. Each vendored checkout records its pin, and one that no longer
# matches stops the configure rather than building something else.
#
# Vendored code builds with warnings off and hidden visibility. It is not
# ours to keep clean, and burying our own warnings under it is how ours get
# ignored.

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/Pins.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/PatchedSource.cmake)

set(SUPPLE_VENDOR_DIR "" CACHE PATH "Build from vendored sources here (tools/vendor.cmake) instead of downloading")
set(supple_vendor "${SUPPLE_VENDOR_DIR}")
if (supple_vendor STREQUAL "" AND EXISTS "${SUPPLE_ROOT}/vendor/.supple-vendor")
    set(supple_vendor "${SUPPLE_ROOT}/vendor")
endif()
if (supple_vendor)
    get_filename_component(supple_vendor "${supple_vendor}" ABSOLUTE BASE_DIR "${SUPPLE_ROOT}")
    if (NOT EXISTS "${supple_vendor}/.supple-vendor")
        message(FATAL_ERROR "${supple_vendor} is not a vendor folder; make one with "
                            "cmake -DDIR=${supple_vendor} -P framework/tools/vendor.cmake")
    endif()
    message(STATUS "Building from the vendored sources in ${supple_vendor}")
    set(FETCHCONTENT_FULLY_DISCONNECTED ON)
endif()

function(supple_vendored target)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
        OBJC_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W0)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    supple_sanitize(${target})
endfunction()

# Where a vendored source is, checked against its pin; "" when not vendoring.
function(supple_vendored_source name out)
    set(${out} "" PARENT_SCOPE)
    if (NOT supple_vendor)
        return()
    endif()
    set(dir "${supple_vendor}/${name}")
    supple_pin_stamp(${name} want)
    set(have "")
    if (EXISTS "${dir}/.supple-pin")
        file(READ "${dir}/.supple-pin" have)
        string(STRIP "${have}" have)
    endif()
    if (NOT have STREQUAL want)
        message(FATAL_ERROR "The vendored ${name} in ${supple_vendor} is not the pinned one (${want}); "
                            "run cmake -DDIR=${supple_vendor} -P framework/tools/vendor.cmake again")
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

# Declares a pinned dependency, from its vendored copy when there is one. A
# dependency with patches (Pins.cmake) never has them applied to a source
# outside the build directory -- a vendored copy, or one given with
# FETCHCONTENT_SOURCE_DIR_<NAME> -- but to a copy of it in the build.
function(supple_declare name)
    cmake_parse_arguments(D "HEADER_ONLY" "" "" ${ARGN})
    list(GET SUPPLE_PIN_${name} 0 url)
    list(GET SUPPLE_PIN_${name} 1 ref)
    string(TOUPPER "${name}" upper)
    set(args GIT_REPOSITORY ${url} GIT_TAG ${ref} GIT_SHALLOW OFF)
    if (D_HEADER_ONLY)
        list(APPEND args SOURCE_SUBDIR none)  # nothing of its own CMake is used
    endif()
    FetchContent_Declare(${name} ${args})

    supple_vendored_source(${name} source)
    if (NOT source AND DEFINED FETCHCONTENT_SOURCE_DIR_${upper})
        set(source "${FETCHCONTENT_SOURCE_DIR_${upper}}")
    endif()
    if (source AND SUPPLE_PATCHES_${name})
        set(copy "${CMAKE_BINARY_DIR}/_deps/${name}-patched")
        supple_copy_for_patch("${source}" "${copy}" "${SUPPLE_PATCHES_${name}}" "${supple_vendor}")
        set(source "${copy}")
    endif()
    if (source)
        set(FETCHCONTENT_SOURCE_DIR_${upper} "${source}" PARENT_SCOPE)
    endif()
endfunction()

# Applies a patch to a fetched checkout, idempotently: a reconfigure finds it
# already applied and moves on; a dependency that moved so the patch no longer
# applies stops the configure rather than building unpatched. Each patch is
# also a test (patch-<name>), so a dependency bump cannot drop one quietly.
function(supple_apply_patch source_dir patch)
    get_filename_component(name "${patch}" NAME_WE)
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --check "${patch}"
        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE applicable OUTPUT_QUIET ERROR_QUIET)
    if (applicable EQUAL 0)
        execute_process(COMMAND ${GIT_EXECUTABLE} apply "${patch}"
            WORKING_DIRECTORY "${source_dir}" COMMAND_ERROR_IS_FATAL ANY)
        message(STATUS "Applied ${name}")
    else()
        execute_process(COMMAND ${GIT_EXECUTABLE} apply --reverse --check "${patch}"
            WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE present OUTPUT_QUIET ERROR_QUIET)
        if (NOT present EQUAL 0)
            message(FATAL_ERROR "${name} no longer applies to ${source_dir}")
        endif()
    endif()
    set_property(GLOBAL APPEND PROPERTY SUPPLE_PATCHES "${name}|${source_dir}|${patch}")
endfunction()

function(supple_apply_patches name)
    foreach(patch ${SUPPLE_PATCHES_${name}})
        supple_apply_patch(${${name}_SOURCE_DIR} ${SUPPLE_FRAMEWORK}/cmake/patches/${patch})
    endforeach()
endfunction()

# Patches need Git even when every source is vendored.
find_package(Git REQUIRED)

# --- dr_libs: WAV, AIFF and FLAC decoding (dsp/AudioFile.h) ---------------------------

# David Reid's single-file decoders, MIT-0. Every product builds them; one
# that never loads a file links none of it.
supple_declare(dr_libs HEADER_ONLY)
FetchContent_MakeAvailable(dr_libs)
add_library(supple-drlibs STATIC ${SUPPLE_FRAMEWORK}/dsp/DrLibs.c)
target_include_directories(supple-drlibs SYSTEM PUBLIC ${dr_libs_SOURCE_DIR})
set_target_properties(supple-drlibs PROPERTIES C_STANDARD 99)
supple_vendored(supple-drlibs)

# --- clap-wrapper: CLAP SDK, VST3 and AUv2 --------------------------------------------

if (SUPPLE_BUILD_PLUGIN)
    set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # clap-wrapper fetches the CLAP, VST3 and AU SDKs itself, at the pins it
    # names, unless it is handed them.
    supple_vendored_source(clap clap_sdk)
    if (clap_sdk)
        set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
        set(CLAP_SDK_ROOT "${clap_sdk}")
        supple_vendored_source(vst3sdk VST3_SDK_ROOT)
        if (APPLE)
            supple_vendored_source(AudioUnitSDK AUDIOUNIT_SDK_ROOT)
        endif()
    else()
        set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
    endif()
    supple_declare(clap-wrapper)
    FetchContent_MakeAvailable(clap-wrapper)
    # The SDK pins in Pins.cmake are the ones this clap-wrapper fetches, or a
    # vendored build would be made of different SDKs from a downloaded one.
    file(READ "${clap-wrapper_SOURCE_DIR}/cmake/base_sdks.cmake" base_sdks)
    foreach(sdk clap vst3sdk AudioUnitSDK)
        list(GET SUPPLE_PIN_${sdk} 1 ref)
        string(FIND "${base_sdks}" "GIT_TAG \"${ref}\"" at)
        if (at EQUAL -1)
            message(FATAL_ERROR "clap-wrapper no longer pins ${sdk} at ${ref}; update framework/cmake/Pins.cmake")
        endif()
    endforeach()

    # The patches (Pins.cmake lists them):
    # - vst3-all-points: clap-wrapper 0.16.0 keeps only the last VST3
    #   automation point in each parameter queue, so a host's sample-accurate
    #   automation arrives once per block.
    # - vst3-timer-floor: register_timer() clamps every request to 30 ms,
    #   capping a VST3 editor at 33 fps however fast it asked to run.
    # - vst3-self-handle: on Linux the wrapper reopened its own module and
    #   kept the handle, pinning it against the host's dlclose and then
    #   unmapping it from inside its own static destructors at exit; and it
    #   looked up clap_entry with a null handle, which searches the whole
    #   process and can find another plugin's.
    # - vst3-surround: a CLAP surround or ambisonic port became an empty VST3
    #   speaker arrangement; it is now 5.1, 7.1, 7.1.4 or first- to
    #   third-order ACN.
    # - vst3-midi-out: MIDI the plugin sent (controllers, pitch bend,
    #   pressure, program changes) was dropped; it now leaves as VST3 MIDI CC
    #   output events.
    # - vst3-tail-on-main: a tail change (CLAP calls it on the audio thread)
    #   called the host's IComponentHandler there, and without checking the
    #   host had given one; it now goes from the idle loop on the UI thread.
    # - vst3-note-capacity: the notes a host left sounding were kept in a
    #   vector reserved for 32 that grew on the audio thread; it now holds
    #   1024, made at activation, and recycles a slot when full. Polyphonic
    #   pressure for a note it does not know is dropped, not sent half-made.
    # - auv2-restart: request_restart did nothing in AU, so a parameter that
    #   needs one (restartOnChange) never took effect; the wrapper now
    #   deactivates and activates the plugin itself from its idle loop, and a
    #   render that arrives meanwhile outputs silence rather than waiting.
    # - auv2-state-errors: a state the plugin refused to save or load was
    #   reported to the host as saved or restored.
    # - auv2-bypass-sync: the native BypassEffect property followed only the
    #   host setting it; it now follows the bypass parameter however it moves
    #   (automation, the plugin's editor, a loaded state), and tells the host.
    #   The bypass parameter's ID, which SetParameter reads on the host's
    #   automation thread, is atomic, and a rescan no longer clears it.
    # - auv2-rescan-race: every state load rescans the parameters, and the
    #   rescan rewrote each parameter's info, which SetParameter reads
    #   unlocked; now only when it changed. GetParameterList takes the
    #   tree's lock. (au-parameter-race, under tsan, found both.)
    # - auv2-parameter-queue: SetParameter, which hosts call from any thread
    #   and several at once, appended to the event list the audio thread was
    #   rendering from; it now copies the change into a fixed queue under a
    #   spinlock, and the next render takes them (pluginval's parameter
    #   thread safety test crashed on it).
    supple_apply_patches(clap-wrapper)
endif()

# --- MTS-ESP: the microtuning client, for PRODUCT_TUNING -----------------------------------

# The client half only (0BSD). It finds the shared library an MTS-ESP master
# installs -- if there is one -- when the plugin loads, and reads the master's
# tuning tables from it; without one it answers equal temperament.
if (SUPPLE_BUILD_PLUGIN AND PRODUCT_TUNING)
    supple_declare(mts-esp)
    FetchContent_MakeAvailable(mts-esp)
    add_library(supple-mts STATIC ${mts-esp_SOURCE_DIR}/Client/libMTSClient.cpp)
    target_include_directories(supple-mts SYSTEM PUBLIC ${mts-esp_SOURCE_DIR}/Client)
    target_link_libraries(supple-mts PRIVATE ${CMAKE_DL_LIBS})
    supple_vendored(supple-mts)
endif()

# --- pugl, NanoVG, nanovg_metal ------------------------------------------------------------

if (SUPPLE_BUILD_GUI)
    supple_declare(pugl)
    supple_declare(nanovg)
    # SVG for the editor's pictures (gui/Images.h): parsed, never rasterized.
    supple_declare(nanosvg HEADER_ONLY)
    FetchContent_MakeAvailable(pugl nanovg nanosvg)
    # Classes with the same product name may live in both CLAP and VST3
    # modules. NULL selects another module's class; keep creation and cleanup
    # tied to the DLL that registered it, so unloading one cannot strand a
    # window procedure in the other. And on macOS, a drop lands where the
    # mouse would: pugl passed the window's coordinates through unconverted.
    # puglRejectOffer, declared everywhere, was only defined for X11.
    supple_apply_patches(pugl)
    if (SUPPLE_METAL)
        supple_declare(nanovg_metal)
        FetchContent_MakeAvailable(nanovg_metal)
    endif()

    # Objective-C has one flat, process-wide class namespace, and pugl and
    # nanovg_metal are vendored by many plugins, this framework's products
    # included. Two images defining the same class make the runtime keep the
    # first and resolve every later lookup by name to it -- fine while the
    # copies are identical, reading at the wrong offsets once one moves. The
    # class names are ordinary identifiers, so the preprocessor gives each
    # product its own, from its plugin ID -- unique by definition, where two
    # display names may not be: com_suppleaudio_starter_PuglWrapperView and
    # so on. auval reported the collision the moment two products from this
    # lineage were installed.
    set(SUPPLE_OBJC_CLASSES
        PuglWindow PuglWindowDelegate PuglWrapperView
        PuglStubView PuglOpenGLView PuglMetalView PuglCairoView PuglVulkanView
        MNVGbuffers MNVGcontext MNVGtexture)

    string(REGEX REPLACE "[^A-Za-z0-9]" "_" SUPPLE_OBJC_PREFIX "${PRODUCT_ID}")
    if (SUPPLE_OBJC_PREFIX MATCHES "^[0-9]")
        set(SUPPLE_OBJC_PREFIX "_${SUPPLE_OBJC_PREFIX}")
    endif()
    function(supple_objc_prefix target)
        if (APPLE)
            foreach(cls ${SUPPLE_OBJC_CLASSES})
                target_compile_definitions(${target} PRIVATE "${cls}=${SUPPLE_OBJC_PREFIX}_${cls}")
            endforeach()
        endif()
    endfunction()

    # pugl ships a meson build; its source list is small and stable (see
    # pugl/meson.build), so the platform files are compiled directly.
    function(supple_pugl name metal)
        set(src ${pugl_SOURCE_DIR}/src/common.c ${pugl_SOURCE_DIR}/src/internal.c)
        if (APPLE)
            list(APPEND src ${pugl_SOURCE_DIR}/src/mac.m)
            if (metal)
                # pugl has no Metal backend, and no upstream work towards one.
                list(APPEND src ${SUPPLE_FRAMEWORK}/gui/pugl_metal.m)
            else()
                list(APPEND src ${pugl_SOURCE_DIR}/src/mac_gl.m)
            endif()
        elseif (WIN32)
            list(APPEND src ${pugl_SOURCE_DIR}/src/win.c ${pugl_SOURCE_DIR}/src/win_gl.c)
        else()
            list(APPEND src ${pugl_SOURCE_DIR}/src/x11.c ${pugl_SOURCE_DIR}/src/x11_gl.c)
        endif()
        add_library(${name} STATIC ${src})
        target_include_directories(${name} SYSTEM PUBLIC ${pugl_SOURCE_DIR}/include)
        set_target_properties(${name} PROPERTIES C_STANDARD 99)
        target_compile_definitions(${name} PUBLIC PUGL_STATIC)  # no dllimport on Windows
        supple_vendored(${name})
        supple_objc_prefix(${name})
        if (APPLE)
            # pugl manages its own retain/release: ARC must stay off, and the
            # Metal backend, built from pugl's internals, follows it.
            set_source_files_properties(${pugl_SOURCE_DIR}/src/mac.m ${pugl_SOURCE_DIR}/src/mac_gl.m
                ${SUPPLE_FRAMEWORK}/gui/pugl_metal.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
            target_link_libraries(${name} PUBLIC "-framework Cocoa" "-framework AppKit"
                "-framework Foundation" "-framework CoreVideo")
            if (metal)
                target_include_directories(${name} PRIVATE ${pugl_SOURCE_DIR}/src ${SUPPLE_FRAMEWORK}/gui)
                target_link_libraries(${name} PUBLIC "-framework Metal" "-framework QuartzCore")
            else()
                target_compile_definitions(${name} PUBLIC GL_SILENCE_DEPRECATION)
                target_link_libraries(${name} PUBLIC "-framework OpenGL")
            endif()
        elseif (WIN32)
            target_link_libraries(${name} PUBLIC dwmapi gdi32 shell32 shlwapi user32 opengl32)
        else()
            find_package(X11 REQUIRED)
            find_package(OpenGL REQUIRED)
            target_compile_definitions(${name} PRIVATE _POSIX_C_SOURCE=200809L)
            target_link_libraries(${name} PUBLIC X11::X11 OpenGL::GL)
            # Optional X extensions, probed as pugl's own build does. The
            # USE_* defines must be present either way.
            foreach(ext Xcursor Xrandr Xext)
                if (X11_${ext}_FOUND)
                    target_link_libraries(${name} PUBLIC X11::${ext})
                endif()
            endforeach()
            target_compile_definitions(${name} PRIVATE
                USE_XCURSOR=$<BOOL:${X11_Xcursor_FOUND}>
                USE_XRANDR=$<BOOL:${X11_Xrandr_FOUND}>
                USE_XSYNC=$<BOOL:${X11_Xext_FOUND}>)
        endif()
    endfunction()

    supple_pugl(supple-pugl ${SUPPLE_METAL})
    if (SUPPLE_METAL)
        supple_pugl(supple-pugl-gl OFF)  # the offline preview reads GL framebuffers
    endif()

    add_library(supple-nanovg STATIC ${nanovg_SOURCE_DIR}/src/nanovg.c)
    target_include_directories(supple-nanovg SYSTEM PUBLIC ${nanovg_SOURCE_DIR}/src)
    set_target_properties(supple-nanovg PROPERTIES C_STANDARD 99)
    supple_vendored(supple-nanovg)

    if (SUPPLE_METAL)
        add_library(supple-nanovg-mtl STATIC ${nanovg_metal_SOURCE_DIR}/src/nanovg_mtl.m)
        target_include_directories(supple-nanovg-mtl SYSTEM PUBLIC ${nanovg_metal_SOURCE_DIR}/src)
        target_link_libraries(supple-nanovg-mtl PUBLIC supple-nanovg "-framework Metal" "-framework QuartzCore")
        supple_vendored(supple-nanovg-mtl)
        supple_objc_prefix(supple-nanovg-mtl)
        # nanovg_mtl.m REQUIRES ARC -- the opposite of pugl. It has no retain,
        # release or dealloc anywhere, passes ownership through NanoVG's void*
        # with __bridge_retained/__bridge_transfer, and assigns autoreleased
        # objects straight to strong ivars. Built without ARC it compiles
        # cleanly and then frees those ivars at the next autorelease pool
        # drain: a use-after-free mid-render. CMake does not enable ARC itself.
        set_source_files_properties(${nanovg_metal_SOURCE_DIR}/src/nanovg_mtl.m
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()

    add_library(supple-nanosvg STATIC ${SUPPLE_FRAMEWORK}/gui/nanosvg.c)
    target_include_directories(supple-nanosvg SYSTEM PUBLIC ${nanosvg_SOURCE_DIR}/src)
    set_target_properties(supple-nanosvg PROPERTIES C_STANDARD 99)
    supple_vendored(supple-nanosvg)

    # Fonts, compiled in: a plugin has nowhere reliable to load a file from.
    # One library, so no generated source belongs to two targets (which races
    # under Make and is an error under Xcode).
    set(font_dir ${SUPPLE_FRAMEWORK}/gui/fonts)
    set(font_out ${CMAKE_BINARY_DIR}/generated/fonts)
    set(font_sources "")
    foreach(spec
            "Inter-Regular:supple_font_inter_regular"
            "Inter-SemiBold:supple_font_inter_semibold"
            "PlexMono-Regular:supple_font_plex_mono_regular"
            "PlexMono-Medium:supple_font_plex_mono_medium"
            "PlexSansCondensed-SemiBold:supple_font_plex_sans_condensed_semibold")
        string(REPLACE ":" ";" parts "${spec}")
        list(GET parts 0 file)
        list(GET parts 1 symbol)
        add_custom_command(OUTPUT ${font_out}/${file}.c
            COMMAND ${CMAKE_COMMAND} -DINPUT=${font_dir}/${file}.ttf -DOUTPUT=${font_out}/${file}.c
                    -DSYMBOL=${symbol} -P ${SUPPLE_FRAMEWORK}/cmake/Bin2c.cmake
            DEPENDS ${font_dir}/${file}.ttf ${SUPPLE_FRAMEWORK}/cmake/Bin2c.cmake
            COMMENT "Embedding ${file}.ttf" VERBATIM)
        list(APPEND font_sources ${font_out}/${file}.c)
    endforeach()
    add_library(supple-fonts STATIC ${font_sources})
    supple_vendored(supple-fonts)
endif()
