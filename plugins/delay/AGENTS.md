# Instructions for AI agents

This repository is a verified starting point, not an example to rewrite from
memory. Preserve the infrastructure until the new product has tests proving a
replacement is better. Read `CONVENTIONS.md`, `BUILDING.md` and the product
identity checklist below before implementation.

## First actions for a new plugin

1. Establish the product name, reverse-DNS plugin ID, semantic version,
   permanent AU subtype code, effect category and supported bus
   layouts. Apply every rename in the product identity checklist below in one
   focused commit.
2. Describe the DSP signal flow, parameter semantics, latency changes and
   stateful behavior before editing the wrapper or GUI.
3. Implement DSP only in the SDK-free core. Keep one implementation shared by
   offline tests and CLAP/VST3/AUv2.
4. Extend the parameter table by appending stable IDs. The wrapper and editor
   must not define duplicate names, ranges, defaults, units or value text.
5. Add deterministic DSP tests and rendered-audio measurements before claiming
   a feature is transparent, analog, true peak, alias free, lossless or exact.
6. Adapt the editor to the product rather than merely relabeling this generic
   panel. Preserve keyboard access, balanced gestures, contrast, resize/DPI
   behavior, frame-rate-independent time and renderer isolation. Rewrite
   `Theme.h`; keep `Widgets.h` shared, and keep the token contract in
   `CONVENTIONS.md` satisfied.
7. Run all five ctest tests, the sanitizer preset, the real CLAP host probe and
   the format validators. `host-smoke` needs its compile definitions set for
   the new product before it means anything -- see the product identity
   checklist below. A preview image proves drawing only. Never describe
   Linux/Windows host behavior as verified from a macOS build.
8. When a fix belongs to the shared stack rather than the product, make it in
   the template and carry it to every product that has the same file. The
   shared GUI stack is kept byte-identical apart from the product namespace
   and license header, and drifting copies are how the same bug gets fixed
   three times and missed the fourth.

## Monorepo override

Keep MIT under ADR-0011. Never modify the upstream template. Repository licensing and user instructions override the upstream checklist.

## Product identity checklist

Perform this before substantive DSP work. Search the complete tree (excluding
build directories) after renaming; no placeholder should survive accidentally.

### Required decisions

- Product display name and lowercase code/namespace name.
- Reverse-DNS plugin identifier, `com.suppleaudio.<product>`. The vendor is
  Supple Audio, already set throughout.
- Project version and product description.
- A unique AU subtype code: exactly four ASCII characters. The manufacturer
  code `Supl` is Supple Audio's and is shared by every product, so the subtype
  alone tells products apart. Both identify saved sessions; never change them
  after release.
- Plugin features/category, bus layouts and whether the product is an effect or
  instrument.
- Product license. The template is MIT; Supple Audio products are
  GPL-3.0-or-later. Replace `LICENSE` with the GPLv3 text, change every
  `SPDX-License-Identifier: MIT` header to `GPL-3.0-or-later` (keeping
  `AND ISC` on `pugl_metal.{h,m}`), switch the copyright lines to
  `Copyright (C) <year> Supple Audio`, and add a License section to the
  README. The build already stages `LICENSE` beside `NOTICES.txt`.

### Placeholder tokens

Replace identifiers deliberately, respecting case:

- `Delay` — display/type/file-name form.
- `delay` — namespace, target and symbol form.
- `DELAY` — CMake option and preprocessor form.
- `com.suppleaudio.delay` — CLAP and bundle identifier (rename only the
  last component).
- `QDly` — AU subtype code.

On macOS the `Delay` form additionally namespaces the Objective-C classes that
pugl and nanovg_metal define (`DELAY_OBJC_CLASSES` in `plugin/gui/CMakeLists.txt`,
applied as `-DPuglWrapperView=Delay_PuglWrapperView` and so on). Objective-C has
one flat, process-wide class namespace, so two products that both keep the
placeholder prefix will collide in any host that loads them together: the runtime
warns, keeps whichever loaded first, and every subsequent lookup by name resolves
to that image. Renaming `Delay` throughout carries this along automatically --
just do not special-case those lines out.

Also rename `DelayParams.h`, the entry/wrapper source files and the DSP include
directory. Update the state magic in `delay_clap.cpp` to a new four-byte tag;
once a product ships, retain its old versions and migration rules.

The fonts are registered under product-prefixed names (`delay-mono`,
`delay-mono-medium`, `delay-display`, `delay-ui`, and
`delay-ui-semibold` in `Theme.h`). These follow the `delay` rename
automatically; keep the names in `Theme.h` aligned with the embedded font
symbols in `plugin/gui/CMakeLists.txt` and `Editor.cpp`. Inter is used for UI
labels, Plex Mono for values, and Plex Sans Condensed remains available for
display type. Check the selected font's glyph coverage before adding symbols.

Two things need a decision rather than a rename:

- **`host-smoke` needs to be told what to drive.** In `CMakeLists.txt`, set
  `HOST_SMOKE_PLUGIN_ID` to the new CLAP id and `HOST_SMOKE_PLAIN_PARAM` to the
  display name of a parameter that must never change latency. If the product
  has a parameter that *does* change latency, name it in
  `HOST_SMOKE_LATENCY_PARAM` as well; if it does not, leave that undefined and
  the restart section is skipped rather than faked. The test detects an
  instrument on its own, from a declared audio-input count of zero.
- **`Theme.h` is the file a product is supposed to rewrite**, but the token set
  is a contract with the shared widgets: `ink`, `panel`, `well`, `edge`,
  `edgeHi`, `dim`, `dust`, `bone`, `accent`, `alert`, and the four knob tones
  `knobWell`, `knobTop`, `knobBot`, `knobRim`. Give them the new product's
  values; do not delete any of them, and do not hard-code a colour in
  `Widgets.h` to avoid adding one. See `CONVENTIONS.md`.

Finally delete the old build directory and configure from scratch. Wrapper and
AU metadata are generated at configure time, so an incremental build can retain
stale identity even when source strings have changed.

## Non-negotiable implementation rules

- The audio thread does not allocate, lock, log, access files, call the GUI, or
  rebuild expensive topology during ordinary processing. Size scratch storage
  in activation/prepare. If a genuine bus-layout mismatch forces re-prepare,
  make that exceptional path explicit.
- `prepare -> setParams` must snap restored/session values into smoothers.
  Later updates smooth normally. Re-prepare must be idempotent.
- Process automation at event sample offsets. Do not silently reduce CLAP
  automation to one value per host block.
- Parameter IDs are session ABI. Append; never reorder, recycle or renumber.
  State is versioned ID/value data, not a raw struct dump.
- Parameters that change latency or topology require an explicit host-contract
  design and restart policy. Do not add them casually.
- Editor-to-audio and audio-to-editor channels remain bounded SPSC queues.
  On overflow, drop the new item; never overwrite an object the consumer may
  be copying. Shared flags/values are atomic.
- Generate telemetry only while an editor exists. Batch it at control rate and
  meter the raw state that matches the UI's label. Telemetry must not change
  audio output or host-block invariance.
- A mouse wheel edit is one host gesture, closed after 250 ms of inactivity.
  Drag, keyboard and focus-loss paths must always balance begin/end.
- Animation and peak decay use elapsed seconds supplied by `Window`, not a
  presumed 60 Hz frame count.
- Keep Metal, OpenGL and platform headers out of editor code. `Window.cpp`
  calls the small `delay*` backend seam; exactly one renderer translation
  unit supplies it.
- Pugl and the local Metal backend use manual retain/release. `nanovg_mtl.m`
  requires ARC. Do not unify those compiler settings.
- Do not copy GPL DSP or source from another plugin. Papers, measurements and
  high-level ideas can guide an original implementation; record provenance and
  license checks for any new dependency.

## Completion standard

Do not stop at “it compiles.” A product change is complete when behavior is
measured, tests cover lifecycle and block boundaries, every affected format is
built from the same source, the real editor opens and survives resize/reopen/
teardown, documentation matches the code, and remaining platform uncertainty
is stated plainly.
