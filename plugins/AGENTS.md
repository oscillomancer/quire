# Working on the stock plugins

The rules in the root `AGENTS.md` apply here too. This file adds what is
specific to `plugins/`.

## Layout

Stock plugins written as DSP scripts live in `stock/` (ADR-0047): one
folder each, holding the script and the product folder a source export
makes from it, built from `template/` with `suite/`. `stock/README.md` says
how to change one. Yarrow (`stock/yarrow`, the EQ), Sundew
(`stock/sundew`, the compressor), Treeline (`stock/treeline`, the
limiter), Gorse (`stock/gorse`, the transient shaper), Hickory
(`stock/hickory`, the stereo utility), Sorrel (`stock/sorrel`, the gate),
Teasel (`stock/teasel`, the saturation), Bracken (`stock/bracken`, the
delay) and Canopy (`stock/canopy`, the reverb) are written so far; the rest of this section is about the older,
hand-written plugins.

Each plugin directory (`analyzer`, `orrery`,
and the internal test instrument `probe-synth`) is an instance of Supple Audio's plugin template (ADR-0011):

- `engine/`: the SDK-free DSP core and the parameter table (core-params
  `ParamSpec`s), with its own tests. It builds as part of the monorepo and
  runs in every sanitizer preset.
- `plugin/`: the CLAP wrapper layer and the editor (`plugin/gui/`).
- `test/`: host smoke tests, semantic-parameter conformance, editor input,
  export and wrapper checks.
- `cmake/Identity.cmake`: the display name, the only place it is written.
- `QUIRE.md`: how this plugin departs from the template and what it does
  that is not obvious from the code. Read it before changing the plugin.
- `README.md`, `BUILDING.md` (building the plugin on its own),
  `CONVENTIONS.md` (the template's coding, DSP and GUI conventions),
  `VERIFICATION.md` (where present: measurements behind the plugin's
  claims), `NOTICES.txt` and `LICENSE` (staged beside the built plugin).

`common/` holds the shared semantic-parameters extension
(`com.suppleaudio.semantic-params`, spec in
`docs/specs/semantic-params-extension.md`) and the CMake glue that links
plugins against the monorepo's libraries.

The monorepo builds each plugin as an isolated ExternalProject
(`plugins/CMakeLists.txt`); its own test suite runs as
`plugin.<dir>.ctest`. Built plugins land in `build/<preset>/plugins/clap/`.

`template/` is the plugin template at a released framework version, a
byte-identical copy (`git archive` of the template's commit): change the
template first, then copy it here, never edit it in place. `suite/` is DSP
scripting (ADR-0032): the script compiler and runtime, the semantic layer
(the extension as a framework layer), and the player, a run-time product
that `plugins/CMakeLists.txt` builds from `template/` with
`-DSUPPLE_PRODUCT_DIR=suite/player` and this repository's `third_party`
copies (`supple_add_template_product`).

## Rules

- **Shared stack.** Files that come from the template's shared stack stay
  byte-identical to the template apart from the product namespace and
  license header: the window, renderer backends and Pugl/NanoVG
  integration (`Window.*`, `Gui.h`), `Telemetry.h`, `test/host_contract.h`,
  the clap-wrapper patches and the probes. `tools/ci/format.sh` keeps them
  out of formatting. A fix that belongs there is made in the template and
  then carried to every plugin that has the file, so the copies never
  drift.
- **Product-owned code** is the parameter table, the DSP core, the wrapper's
  params/state/extension sections, `Theme.h`, `Widgets.h`, the editor and
  the identity. Change it freely, with tests. `Theme.h` must keep the token
  set the widgets rely on (`ink`, `panel`, `well`, `edge`, `edgeHi`, `dim`,
  `dust`, `bone`, `accent`, `alert`, `knobWell`, `knobTop`, `knobBot`,
  `knobRim`): give them values, never delete one, and never hard-code a
  colour in `Widgets.h` instead of adding a token (`CONVENTIONS.md`).
- **Permanent identifiers.** The CLAP id (`com.suppleaudio.<id>`), the AU
  subtype, the state magic and every parameter's stable id and key identify
  saved sessions and patches. Never change or reuse them; append new
  parameters with new ids.
- **One parameter table.** Names, ranges, defaults, units, curves and value
  text come from the engine's table only; the wrapper and the editor read
  it. Values in patches and on the bus are physical units (ADR-0010).
- **Determinism.** A render depends only on the plugin's state and input:
  seeded randomness, identical output across block sizes, and nothing left
  over after `reset()` (each plugin's reset test checks this).
- **Real time.** `process` and everything it calls follow the root rule:
  no allocation, locks or I/O. State and patch loads are transactional:
  parse and validate fully, then apply.

## Implementation rules from the template

- Size scratch storage in activation/prepare. A re-prepare forced by a real
  bus-layout change is an explicit, exceptional path.
- `prepare -> setParams` snaps restored values into the smoothers; later
  updates smooth normally. Re-prepare is idempotent.
- Automation is processed at event sample offsets, never reduced to one
  value per host block.
- Parameters that change latency or topology need an explicit restart
  policy (and `HOST_SMOKE_LATENCY_PARAM` set in the plugin's
  `CMakeLists.txt` so `host-smoke` exercises it).
- Editor-to-audio and audio-to-editor channels are bounded SPSC queues
  that drop the new item on overflow. Telemetry is produced only while an
  editor exists, at control rate, and never changes the audio.
- A mouse-wheel edit is one host gesture, closed after 250 ms of
  inactivity. Drag, keyboard and focus-loss paths always balance begin and
  end.
- Animation and meter decay use elapsed seconds from `Window`, not a
  presumed frame rate.
- Editor code includes no Metal, OpenGL or platform headers; `Window.cpp`
  calls the small backend seam, and one renderer translation unit supplies
  it. Check the font's glyph coverage before adding a symbol.
- A change is finished when its behaviour is measured, tests cover
  lifecycle and block boundaries, every built format comes from the same
  source, and the real editor opens, resizes, reopens and tears down.

## Specs and guides

DSP and patch contracts: `docs/specs/effects.md`,
`docs/specs/stock-effects.md`, `docs/specs/synth.md` (Orrery). The agent
guides in `skill/guides/<id>.md` describe each plugin's patches for agents;
update them when a patch format or a parameter's meaning changes.

## Checks

The plugin suites run in every preset. Beyond them:
`tools/validator/run.sh build/dev/plugins/clap/<Name>.clap` (clap-validator)
and `python3 tools/reaper-check/check.py --build-dir build/dev --plugin
<CLAP id>` (headless REAPER load, render, automation, state; `--editor`
opens the editor).
