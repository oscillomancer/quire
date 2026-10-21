# Working on the stock plugins

The rules in the root `AGENTS.md` apply here too. This file adds what is
specific to `plugins/`.

## Layout

Every stock plugin is a DSP script (ADR-0047) in `stock/`: one folder each,
holding the script and the product folder a source export makes from it,
built from `template/` with `suite/`. `stock/README.md` says how to change
one. Yarrow (`stock/yarrow`, the EQ), Sundew (`stock/sundew`, the
compressor), Treeline (`stock/treeline`, the limiter), Gorse
(`stock/gorse`, the transient shaper), Hickory (`stock/hickory`, the stereo
utility), Sorrel (`stock/sorrel`, the gate), Teasel (`stock/teasel`, the
saturation), Bracken (`stock/bracken`, the delay), Canopy (`stock/canopy`,
the reverb), Lichen (`stock/lichen`, the analyzer) and Marram
(`stock/marram`, the synthesizer). Orrery, the hand-written synthesizer
Marram replaced, was removed on 2026-10-21; git history has it.

`common/` holds the shared semantic-parameters extension
(`com.suppleaudio.semantic-params`, spec in
`docs/specs/semantic-params-extension.md`), the plugin test tools
(`clap-reset-check`, `clap-unload-probe`, `semantic-conformance`) and the
CMake glue that links plugins against the monorepo's libraries.

The monorepo builds each plugin as an isolated ExternalProject
(`plugins/CMakeLists.txt`, `supple_add_template_product` and
`supple_add_stock_script`); its own test suite runs as
`plugin.<name>.ctest`. Built plugins land in `build/<preset>/plugins/clap/`.

`template/` is the plugin template at a released framework version, a
byte-identical copy (`git archive` of the template's commit): change the
template first, then copy it here, never edit it in place. `suite/` is DSP
scripting (ADR-0032): the script compiler and runtime, the semantic layer
(the extension as a framework layer), and the player, a run-time product
that `plugins/CMakeLists.txt` builds from `template/` with
`-DSUPPLE_PRODUCT_DIR=suite/player` and this repository's `third_party`
copies.

## Rules

- **Shared stack.** The template's shared stack (the window, renderer
  backends, Pugl/NanoVG integration, `Telemetry.h`, the host contract, the
  clap-wrapper patches and the probes) is the template's own: a fix that
  belongs there is made in the template and copied here, so no copy drifts.
- **Permanent identifiers.** The CLAP id (`com.suppleaudio.<id>`), the AU
  subtype, the state tag and every parameter's key identify saved sessions
  and patches. Never change or reuse them; add new parameters with new keys.
- **One parameter table.** Names, ranges, defaults, units and value text
  come from the script's parameters only. Values in patches and on the bus
  are physical units (ADR-0010).
- **Determinism.** A render depends only on the plugin's state and input:
  seeded randomness, identical output across block sizes, and nothing left
  over after `reset()` (each plugin's reset test checks this).
- **Real time.** `process` and everything it calls follow the root rule:
  no allocation, locks or I/O; the script compiler enforces it for scripts.

## Specs and guides

DSP and patch contracts: `docs/specs/stock-effects.md` (the effects) and
`docs/specs/marram.md` (the synthesizer). The agent
guides in `skill/guides/<id>.md` describe each plugin's patches for agents;
update them when a patch format or a parameter's meaning changes.

## Checks

The plugin suites run in every preset. Beyond them:
`tools/validator/run.sh build/dev/plugins/clap/<Name>.clap` (clap-validator)
and `python3 tools/reaper-check/check.py --build-dir build/dev --plugin
<CLAP id>` (headless REAPER load, render, automation, state; `--editor`
opens the editor).
