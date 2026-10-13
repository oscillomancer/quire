/*
 * CLAP extension: com.suppleaudio.editor-picture/1
 * Spec: docs/specs/editor-picture-extension.md.
 *
 * Lets a host see a plugin's editor without opening a window: a picture
 * drawn offscreen, for an agent or a test that cannot look at a screen.
 * Hosts that do not know it lose nothing.
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

    /* [main-thread] Draws the editor as it would open now, `scale` times its
     * design size (clamped to 0.25 .. 4), and writes it to `out`: its width
     * and height in pixels as two little-endian uint32, then width * height
     * pixels of 8-bit RGBA, rows from the top. Returns false, writing
     * nothing to `out`, when the plugin cannot draw without a window here;
     * then `why`, if not NULL, receives a line of UTF-8 saying why. */
    bool(CLAP_ABI* draw)(const clap_plugin_t* plugin, double scale, const clap_ostream_t* out,
                         const clap_ostream_t* why);
} clap_plugin_supple_editor_picture_t;

#ifdef __cplusplus
}
#endif
