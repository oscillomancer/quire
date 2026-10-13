# Spec: CLAP extension `com.suppleaudio.editor-picture/1`

**Status:** Implemented. The extension ID carries the major version;
incompatible changes get `/2`. Additions within `/1` only append members
after a version check (§4).
**Header:** `plugins/common/include/supple/clap/editor_picture.h` (C ABI,
depends only on the CLAP headers), with a byte-identical copy in
`plugins/suite/semantic/include/supple/clap/` so that source exports build
on their own.
**Implementations:** plugin side in the suite layer
(`plugins/suite/semantic/src/semantic_layer.cpp`, over the plugin
template's `Instance::watch()` and `Instance::drawEditor()`, framework
1.20.0); host side in `host/clap-host` (`Instance::watchEditor`,
`Instance::drawEditor`), used by `dsp.preview`.

## 1. Purpose

An agent working through Quire's MCP server cannot look at a plugin window.
This extension lets a host ask a plugin for a picture of its editor, drawn
offscreen with no window and no display, as it would look if opened now:
its controls at their values, its meters and the plugin's own displays
showing audio the host has just processed. `dsp.preview` saves it as a PNG,
which the agent reads. Hosts that do not know the extension lose nothing.

## 2. Plugin interface

```c
static const char CLAP_EXT_SUPPLE_EDITOR_PICTURE[] = "com.suppleaudio.editor-picture/1";

typedef struct clap_plugin_supple_editor_picture {
    uint32_t struct_size;
    void (*watch)(const clap_plugin_t* plugin, bool on);
    bool (*draw)(const clap_plugin_t* plugin, double scale, const clap_ostream_t* out,
                 const clap_ostream_t* why);
} clap_plugin_supple_editor_picture_t;
```

All functions are `[main-thread]`.

- **`watch(on)`.** While on, the plugin keeps what its editor shows of the
  audio up to date as if an editor were open (the template's telemetry and
  audio feed; a script's `Shared` members), so that audio processed
  meanwhile shows in a picture. A plugin may cost a little more per block
  while watched. Off by default; the host turns it off when done. Watching
  changes no audio.
- **`draw(scale, out, why)`.** Draws the editor as it would open now at
  `scale` times its design size (clamped to 0.25 .. 4) and writes to `out`:
  - the width and height in pixels, each a little-endian `uint32`;
  - then `width * height` pixels of 8-bit RGBA, rows from the top.

  It returns false, writing nothing to `out`, when the plugin cannot draw
  without a window here (no offscreen renderer on the platform, no EGL, a
  build without an editor). `why`, if not null, then receives one line of
  UTF-8 saying why. Drawing uses a context and a thread of its own: a GL
  context the host has current stays current. An open editor is not
  disturbed, though the picture's editor may take telemetry frames the
  open one would have shown.

The picture is not deterministic across machines (GPU or software
rasterisers differ in antialiasing); hosts compare pictures loosely, if at
all.

## 3. Host behaviour

`dsp.preview` (engine and sandbox) makes a fresh in-process instance, sets
its patch, calls `watch(true)`, renders a test signal or notes through it
with the offline renderer, calls `draw`, then `watch(false)`, and writes the
pixels as a PNG in the renders folder. Isolated (bridged) instances are not
pictured.

## 4. Versioning

`struct_size` is the size of the struct the plugin was built with; a host
calls no member beyond it, and a plugin whose struct is smaller than this
version's is treated as not offering the extension. New members are only
appended.
