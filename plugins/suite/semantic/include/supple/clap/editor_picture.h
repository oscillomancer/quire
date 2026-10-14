// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
/*
 * CLAP extension: com.suppleaudio.editor-picture/1
 * Spec: docs/specs/editor-picture-extension.md.
 *
 * Lets a host see a plugin's editor without opening a window: a picture
 * drawn offscreen, for an agent or a test that cannot look at a screen,
 * after clicks, drags and turns of the wheel if it asks, so it can check
 * what they do. Hosts that do not know it lose nothing.
 *
 * C ABI; depends only on the CLAP headers. All functions are main-thread.
 */
#pragma once

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const CLAP_CONSTEXPR char CLAP_EXT_SUPPLE_EDITOR_PICTURE[] =
    "com.suppleaudio.editor-picture/1";

/* What the pointer does: moves to (x, y) (a drag while the main button is
 * down), presses or releases the main button there, turns the wheel there,
 * or leaves the editor. */
enum {
    CLAP_SUPPLE_POINTER_MOVE = 0,
    CLAP_SUPPLE_POINTER_PRESS = 1,
    CLAP_SUPPLE_POINTER_RELEASE = 2,
    CLAP_SUPPLE_POINTER_WHEEL = 3,
    CLAP_SUPPLE_POINTER_LEAVE = 4,
};

/* Keys held, and a press that is the second of a double click. */
enum {
    CLAP_SUPPLE_POINTER_SHIFT = 1 << 0,
    CLAP_SUPPLE_POINTER_CTRL = 1 << 1, /* Cmd on a Mac */
    CLAP_SUPPLE_POINTER_ALT = 1 << 2,
    CLAP_SUPPLE_POINTER_DOUBLE_CLICK = 1 << 3,
};

typedef struct clap_supple_pointer_input {
    uint32_t kind;  /* CLAP_SUPPLE_POINTER_* */
    uint32_t flags; /* CLAP_SUPPLE_POINTER_SHIFT | ... */
    /* In the editor's design units: the picture's pixels at scale 1. */
    double x, y;
    double wheel; /* WHEEL: notches turned, positive up (away from the user) */
} clap_supple_pointer_input_t;

typedef struct clap_plugin_supple_editor_picture {
    /* sizeof(clap_plugin_supple_editor_picture_t) of the header the plugin was
     * built with. Hosts must not call members beyond it; new members are only
     * ever appended. */
    uint32_t struct_size;

    /* [main-thread] While on, the plugin keeps what its editor shows of the
     * audio (meters, scopes, the plugin's own displays) up to date as if an
     * editor were open, so that audio processed meanwhile shows in a
     * picture. Off by default; turn it off when done. */
    void(CLAP_ABI* watch)(const clap_plugin_t* plugin, bool on);

    /* [main-thread] Makes the editor as it would open now, gives it `count`
     * pointer inputs in order (`input` may be NULL when `count` is 0), as a
     * user's mouse would, then draws it at `scale` times its design size
     * (clamped to 0.25 .. 4) and writes it to `out`: its width and height in
     * pixels as two little-endian uint32, then width * height pixels of
     * 8-bit RGBA, rows from the top. What the input edits reaches the host
     * as a user's edits do, each gesture closed before draw returns.
     * Returns false, writing nothing to `out` and applying no input, when
     * the plugin cannot draw without a window here; then `why`, if not
     * NULL, receives a line of UTF-8 saying why. */
    bool(CLAP_ABI* draw)(const clap_plugin_t* plugin, double scale,
                         const clap_supple_pointer_input_t* input, uint32_t count,
                         const clap_ostream_t* out, const clap_ostream_t* why);
} clap_plugin_supple_editor_picture_t;

#ifdef __cplusplus
}
#endif
