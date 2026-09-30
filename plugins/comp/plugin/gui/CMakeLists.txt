# Comp GUI: pugl for the embedded window and event plumbing, NanoVG for the
# vector rendering. Both are permissively licensed (ISC / zlib), compatible
# with the template's MIT license, and dependency free beyond system libraries.
#
# pugl ships a meson build; rather than drag meson into the toolchain we compile
# its handful of platform files directly. The file list is small and stable
# (see pugl/meson.build): two common sources plus one platform pair.

include(FetchContent)

# Apple deprecated OpenGL in 2018. It still works -- macOS 26 Tahoe removed AGL
# but not GL, and pugl uses NSOpenGLContext rather than AGL -- but the
# direction is not in doubt, so macOS renders through Metal by default and GL
# remains the only backend elsewhere.
option(COMP_METAL "Render through Metal instead of OpenGL on macOS" ON)
if (NOT APPLE)
    set(COMP_METAL OFF)
endif()

FetchContent_Declare(pugl
    GIT_REPOSITORY https://github.com/lv2/pugl.git
    GIT_TAG b7637149ebe53124e5be90559e02a0185bbcbd73
    GIT_SHALLOW OFF)
FetchContent_MakeAvailable(pugl)

FetchContent_Declare(nanovg
    GIT_REPOSITORY https://github.com/memononen/nanovg.git
    GIT_TAG ce3bf745eb2d2dbc14a50bf2446783f691ac4353
    GIT_SHALLOW OFF)
FetchContent_MakeAvailable(nanovg)

# NanoVG's Metal renderer. This is wjakob's fork rather than ollix/MetalNanoVG,
# which has had no commit since September 2022: the fork adds an explicit
# command queue and target texture, and sizes the stencil buffer from the
# render target, which is what NVG_STENCIL_STROKES needs to behave. It is a
# standalone renderer rather than a fork of NanoVG itself, so it drops in
# alongside the pinned upstream above. Shaders ship as precompiled bitcode
# headers, so there is no .metal compilation step to own.
if (COMP_METAL)
    FetchContent_Declare(nanovg_metal
        GIT_REPOSITORY https://github.com/wjakob/nanovg_metal.git
        GIT_TAG 1e4243990741f5b0da73bc0f25c85ae0e31b8766
        GIT_SHALLOW OFF)
    FetchContent_MakeAvailable(nanovg_metal)
endif()

# ---------------------------------------------------------------------------
# pugl
# ---------------------------------------------------------------------------

# Built as a function so a Metal build can also produce a GL variant for the
# offline preview tool. The two differ only on Apple; elsewhere GL is the only
# backend and the second target is never created.
# Objective-C has one flat, process-wide class namespace, and every plugin
# built from this template statically links the same pugl and nanovg_metal
# sources. Two of them in one host therefore register the same class names
# twice. The runtime keeps whichever image loaded first, prints "Class X is
# implemented in both ... mysterious crashes", and from then on every lookup by
# name resolves to that first image regardless of which plugin is asking.
#
# Direct references still bind within each image, so identical copies happen to
# behave. What this guards against is the version skew that follows: the moment
# one product's pugl or nanovg_metal moves and an ivar layout changes, two
# same-named classes with different instance sizes coexist and any name-based
# lookup reads at the wrong offsets. It also guards against third-party plugins,
# which vendor pugl just as widely and are outside our control.
#
# This is not hypothetical: auval reported the collision the moment one product
# from this lineage was installed alongside another.
#
# Class names are ordinary identifiers in the source, so renaming them at the
# preprocessor gives this product its own corner of the namespace and leaves the
# vendored sources untouched.
set(COMP_OBJC_CLASSES
    PuglWindow PuglWindowDelegate PuglWrapperView
    PuglStubView PuglOpenGLView PuglMetalView PuglCairoView PuglVulkanView
    MNVGbuffers MNVGcontext MNVGtexture)

function(comp_namespace_objc target)
    if (NOT APPLE)
        return()
    endif()
    foreach(cls ${COMP_OBJC_CLASSES})
        target_compile_definitions(${target} PRIVATE "${cls}=Comp_${cls}")
    endforeach()
endfunction()

function(comp_pugl_target name useMetal)
    set(src ${pugl_SOURCE_DIR}/src/common.c ${pugl_SOURCE_DIR}/src/internal.c)

    if (APPLE)
        list(APPEND src ${pugl_SOURCE_DIR}/src/mac.m)
        if (useMetal)
            # pugl has no Metal backend and no upstream work towards one, so we
            # supply it. See pugl_metal.h.
            list(APPEND src ${CMAKE_CURRENT_SOURCE_DIR}/pugl_metal.m)
        else()
            list(APPEND src ${pugl_SOURCE_DIR}/src/mac_gl.m)
        endif()
    elseif (WIN32)
        list(APPEND src ${pugl_SOURCE_DIR}/src/win.c ${pugl_SOURCE_DIR}/src/win_gl.c)
    else()
        list(APPEND src ${pugl_SOURCE_DIR}/src/x11.c ${pugl_SOURCE_DIR}/src/x11_gl.c)
    endif()

    add_library(${name} STATIC ${src})
    target_include_directories(${name} PUBLIC ${pugl_SOURCE_DIR}/include)
    set_target_properties(${name} PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    # PUGL_STATIC stops the headers asking for dllimport on Windows.
    target_compile_definitions(${name} PUBLIC PUGL_STATIC)

    comp_namespace_objc(${name})

    if (APPLE)
        # pugl manages its own retain/release; ARC must stay off, and our Metal
        # backend follows the same convention because it is built from the same
        # internals.
        set_source_files_properties(
            ${pugl_SOURCE_DIR}/src/mac.m ${pugl_SOURCE_DIR}/src/mac_gl.m
            ${CMAKE_CURRENT_SOURCE_DIR}/pugl_metal.m
            PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        target_compile_options(${name} PRIVATE -Wno-deprecated-declarations)
        target_link_libraries(${name} PUBLIC
            "-framework Cocoa" "-framework AppKit" "-framework Foundation"
            "-framework CoreVideo")
        if (useMetal)
            # pugl_metal.m is built against pugl's private headers, exactly as
            # pugl's own backends are. Note that no GL framework is linked here:
            # a Metal build of the plugin has no OpenGL dependency at all, which
            # is the entire point of having one.
            target_include_directories(${name} PRIVATE
                ${pugl_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
            target_link_libraries(${name} PUBLIC
                "-framework Metal" "-framework QuartzCore")
        else()
            # GL is deprecated on macOS but entirely functional -- silence the
            # noise rather than pretend otherwise.
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

        # Optional X extensions, exactly as pugl's own build probes for them.
        # Each is a nicety (cursor shapes, refresh-rate reporting, frame sync);
        # the USE_* defines must be present either way or pugl will not compile.
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

comp_pugl_target(comp-pugl ${COMP_METAL})
if (COMP_METAL)
    comp_pugl_target(comp-pugl-gl OFF)
endif()

# ---------------------------------------------------------------------------
# NanoVG
# ---------------------------------------------------------------------------

add_library(comp-nanovg STATIC ${nanovg_SOURCE_DIR}/src/nanovg.c)
target_include_directories(comp-nanovg SYSTEM PUBLIC ${nanovg_SOURCE_DIR}/src)
set_target_properties(comp-nanovg PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
# NanoVG bundles stb_truetype/stb_image, which are noisy under our warning level
# and are not ours to fix.
if (NOT MSVC)
    target_compile_options(comp-nanovg PRIVATE -Wno-unused-function -Wno-misleading-indentation)
endif()

if (COMP_METAL)
    add_library(comp-nanovg-mtl STATIC ${nanovg_metal_SOURCE_DIR}/src/nanovg_mtl.m)
    comp_namespace_objc(comp-nanovg-mtl)
    target_include_directories(comp-nanovg-mtl PUBLIC ${nanovg_metal_SOURCE_DIR}/src)
    target_link_libraries(comp-nanovg-mtl PUBLIC
        comp-nanovg "-framework Metal" "-framework QuartzCore")
    set_target_properties(comp-nanovg-mtl PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # nanovg_mtl.m REQUIRES ARC. It contains no retain, release or dealloc
    # anywhere, uses __bridge_retained/__bridge_transfer to pass ownership of
    # the context through NanoVG's void*, and assigns autoreleased objects
    # straight to strong ivars (_cbuffers = [NSMutableArray arrayWithCapacity:]).
    # Built without ARC it compiles clean and then frees those ivars at the next
    # autorelease pool drain, which is a use-after-free in the middle of
    # rendering. CMake does not enable ARC on its own, so it is set explicitly
    # here rather than left to a default -- and note this is the opposite of
    # pugl, which manages its own retain/release and must stay non-ARC.
    set_source_files_properties(${nanovg_metal_SOURCE_DIR}/src/nanovg_mtl.m
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_options(comp-nanovg-mtl PRIVATE
        -Wno-unused-function -Wno-deprecated-declarations)
endif()

# ---------------------------------------------------------------------------
# Embedded fonts
# ---------------------------------------------------------------------------
#
# Plex is subset to the panel's glyphs. Inter UI faces include Latin-1,
# punctuation and the true minus sign; see their OFL notice in fonts/.

set(FONT_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/fonts)
file(MAKE_DIRECTORY ${FONT_GEN_DIR})
set(FONT_SOURCES "")

foreach(font
        "PlexMono-Regular:comp_font_mono"
        "PlexMono-Medium:comp_font_mono_medium"
        "PlexSansCondensed-SemiBold:comp_font_display"
        "Inter-Regular:comp_font_ui"
        "Inter-SemiBold:comp_font_ui_semibold")
    string(REPLACE ":" ";" parts ${font})
    list(GET parts 0 fontFile)
    list(GET parts 1 fontSymbol)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/fonts/${fontFile}.ttf)
    set(gen ${FONT_GEN_DIR}/${fontFile}.c)
    add_custom_command(
        OUTPUT ${gen}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${src} -DOUTPUT=${gen} -DSYMBOL=${fontSymbol}
                -P ${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake
        DEPENDS ${src} ${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake
        COMMENT "Embedding ${fontFile}.ttf"
        VERBATIM)
    list(APPEND FONT_SOURCES ${gen})
endforeach()

# ---------------------------------------------------------------------------
# The editor
# ---------------------------------------------------------------------------

# Exactly one backend TU is compiled in: both define the same comp* symbols,
# which is what lets Window.cpp name neither of them.
function(comp_gui_target name useMetal)
    if (useMetal)
        add_library(${name} STATIC Window.cpp Editor.cpp MetalBackend.m ${FONT_SOURCES})
        target_link_libraries(${name} PUBLIC comp-nanovg-mtl comp-pugl)
        target_compile_definitions(${name} PUBLIC COMP_METAL=1)
        set_source_files_properties(MetalBackend.m
            PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
    else()
        add_library(${name} STATIC Window.cpp Editor.cpp NanoVGBackend.c ${FONT_SOURCES})
        target_compile_definitions(${name} PUBLIC COMP_METAL=0)
        # In a Metal build the GL variant needs the GL pugl; otherwise there is
        # only one pugl and it is already the GL one.
        if (TARGET comp-pugl-gl)
            target_link_libraries(${name} PUBLIC comp-pugl-gl)
        else()
            target_link_libraries(${name} PUBLIC comp-pugl)
        endif()
    endif()
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PUBLIC comp-nanovg comp-dsp)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
endfunction()

comp_gui_target(comp-gui ${COMP_METAL})

# The offline preview tool reads the framebuffer back, which under Metal would
# mean rendering into an MNVGframebuffer rather than the layer's drawable --
# a different draw path for a tool whose purpose is to exercise the normal one.
# It therefore keeps rendering through GL even in a Metal build.
if (COMP_METAL)
    comp_gui_target(comp-gui-gl OFF)
endif()
