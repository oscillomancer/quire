# ADR-0047: Stock plugins written as DSP scripts

**Status:** Accepted, 2026-10-18.

Amends ADR-0011 for the plugins in `plugins/stock/`. Builds on ADR-0032.
Contracts: `docs/specs/stock-effects.md`, `skill/guides/dsp-language.md`.

## Context

The first stock plugins are hand-written instances of the plugin template
(ADR-0011): each folder carries its own copy of the template, a DSP core, a
parameter table, a wrapper and an editor, several thousand lines apiece.
DSP scripting (ADR-0032) now makes a plugin from one C++ file, with the
semantic-params extension, JSON patches, factory patches, a drawn editor
and source exports that build the same plugin natively, bit for bit. A
script is also what an agent working in Quire writes, so stock plugins made
the same way exercise that path and serve as its largest examples.

Quire runs scripts in the player, a JIT. Stock plugins must not need it:
some hosts forbid executable memory, native code is faster, and the stock
plugins ship as CLAP (and VST3, AU) files of their own that other hosts load.

## Decision

1. **Source.** Each new stock plugin is a DSP script,
   `plugins/stock/<name>/script/<name>.cpp`, MIT licensed like the other
   plugins (ADR-0033). It follows the stock conventions: physical units,
   one `.help()` sentence per parameter, groups, neutral defaults, factory
   patches, smoothed levels, and choice keys with a unit before a number
   (`.keys({"db12", ...})`, added to the language for this).
2. **Build.** `plugins/stock/<name>/` is the product folder a source export
   makes from the script (`product.cmake`, `Catalog.cpp`, `Layer.cpp`,
   `Processor.h`, `Theme.h`, `targets.cmake`, `NOTICES.txt`), built from
   `plugins/template` with the suite of `plugins/suite`, as the player is
   (`supple_add_stock_script` in `plugins/CMakeLists.txt`). The plugin is
   the script compiled as C++, with no JIT; it plays the same samples as the
   script in the player. `Catalog.cpp` is the one generated file (the patch
   key of each `Param` member); `plugin.<name>.catalog` checks it against a
   fresh export, and `plugins/stock/catalog.cmake -DUPDATE=ON` rewrites it.
   `product.cmake` is kept by hand (features, AU codes, the smoke-test
   parameter).
3. **Checks.** Every stock check applies: the template's own suite
   (`plugin.<name>.ctest`), the reset and unload checks, semantic
   conformance and clap-validator, plus the plugin's own measurements
   where it has them (`response.cmake`).
4. **Identity.** A stock script's id is `com.suppleaudio.<name>` and its
   state tag the one the player gives that id, so a session saved while
   the script ran in the player loads in the native plugin.
5. **Replacing the template plugins.** The template-instance plugins are
   replaced one at a time, each by a new design under a new name and id.
   The old plugin's folder, build entries and documents go in the same
   change, with no conversion of old sessions (none exist outside tests).
   The first is Yarrow, replacing Facet (`com.suppleaudio.eq`); the second
   Sundew, replacing Ballast (`com.suppleaudio.comp`). A stock plugin with
   no predecessor is added the same way: Treeline, the limiter, the third.
   The fourth is Gorse, the transient shaper, replacing Flint
   (`com.suppleaudio.transient`); the fifth Hickory, the stereo utility,
   replacing Plumb (`com.suppleaudio.utility`); the sixth Sorrel, the gate,
   replacing Sluice (`com.suppleaudio.gate`); the seventh Teasel, the
   saturation, replacing Kiln (`com.suppleaudio.drive`); the eighth
   Bracken, the delay, replacing Echelon (`com.suppleaudio.delay`).
6. **Names and look.** A stock plugin is named after a plant or something
   else of the natural world, not after its effect type (Yarrow, the EQ;
   Sundew, the compressor; Treeline, the limiter; Gorse, the transient
   shaper; Hickory, the stereo utility; Sorrel, the gate; Teasel, the
   saturation; Bracken, the delay).
   Every stock script draws with the scripts' palette (`kScriptTheme` in
   `plugins/suite/glue`): warm and earthy, set apart from the host's
   graphite (walnut panels, a soil-dark floor under graphs, parchment
   text, clay and sage accents), with Fraunces (`serifFont`, framework
   1.25.0) for its name and headings. A script's own colours (band
   colours, curves) stay in that key.
7. **Changing one.** Edit the script, try it in the player under a
   scratch id (two plugins with one id would both be listed), then restore
   the id, run the catalog check and the plugin's tests. A change to the
   language or the suite that a stock script needs is made in
   `plugins/suite` like any other, with its tests.

## Consequences

- One file holds a stock plugin's DSP, parameters, patches and editor;
  what the stock plugins need from the language becomes available to every
  script (choice keys first).
- Editors are drawn with the script canvas: pointer and wheel input with
  modifiers, no keyboard input, text entry or secondary-button menus.
- Script code runs with checked indexing in native builds too; its cost is
  measured like any plugin's.
- A user script may still copy a stock script; it must take another id.
