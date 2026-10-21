# Quire integration notes (Orrery)

Orrery is built from the Supple Audio plugin template. The template
infrastructure — window, renderer, widgets, host contract, probes, wrapper
patches and fonts — is kept byte-identical apart from product naming, except
where listed below. `tools/ci/format.sh` leaves those files alone.

## Monorepo integration

- The build is C++20 and uses the vendored CLAP SDK, clap-wrapper, Pugl and
  NanoVG with downloads disabled. It checks that the vendored wrapper
  already carries the template's three VST3 patches instead of applying
  them.
- `plugins/CMakeLists.txt` builds it as an isolated ExternalProject
  (`supple_add_plugin`) and stages `Orrery.clap` under the monorepo
  build's `plugins/clap/`. CLAP is built by default; VST3 needs
  `SUPPLE_PLUGIN_VST3=ON` (`ORRERY_VST3` inside the plugin build).
- `supple_plugin_add_monorepo_libraries()`
  (`plugins/common/cmake/LinkMonorepo.cmake`) builds the monorepo's core
  libraries and `plugin-support` inside that isolated build, so the plugin
  links `supple::plugin-support` and the core targets with the monorepo's
  definitions. The parent passes `SUPPLE_ROOT` and `SUPPLE_SANITIZE`.
- `tools/ci/format.sh` checks only the product-owned files; the unchanged
  template infrastructure keeps its bytes. The license checker treats the
  local ExternalProject as first-party code, not a download.

## Product-specific

- Identity: display name Orrery, code name `orrery`, CLAP id
  `com.suppleaudio.orrery`, AU subtype `QOry` (manufacturer `Supl`), instrument
  (`aumu`), state magic `QOR1` (in the core).
- The template's MIT code license is kept under the monorepo licensing rule and
  ADR-0011; the template's GPL conversion step does not apply.
- There is no product DSP in this copy. The SDK-free engine
  (`plugins/orrery/engine`) and the CLAP core (`plugins/orrery/clap`: params,
  state, note/audio ports, audio-ports config, the semantic extension, the
  editor bridge) are monorepo libraries, built and tested there with the
  monorepo's warnings and sanitizers; `CMakeLists.txt` adds the same sources to
  this isolated build after `supple_plugin_add_monorepo_libraries()`.
- Note off/choke/poly pressure and supported CLAP expressions use conjunctive
  wildcard key/channel/id matching. Mono/legato held records retain the full
  address and velocity. CLAP pressure/brightness/expression override the voice
  modulation sources; malformed note/parameter/MIDI payloads are ignored.
  Re-prepare clears performance state while keeping the patch.
- `plugin/src/orrery_plugin.cpp` replaces the template's `*_clap.cpp`: it only
  adds `clap.gui` and `clap.timer-support` to the core through
  `supple::orrery::ClapExtensions`, plus the factory and entry points.
- The editor (`plugin/gui/Editor.*`, `Theme.h`) is Orrery's own: the patch as a
  signal-flow diagram, the modulators, an inspector for the selected module
  and the route list. It reads everything from `EditorBridge` and makes every
  structural change as a semantic merge patch (`orrery/edits.h`), so it cannot
  build a patch the codec rejects. It uses the Window's extended input API
  (buttons, modifiers, raw keys, cursor). Discrete arrows step one physical
  value/applicable choice even in fine mode; float arrows use normalized steps.
  Wave navigation and Home/End follow the current oscillator type.
  Design size 1120 × 700.
- `test/editor_input.cpp` drives the editor against the real core (layout
  follows structure, balanced gestures including wheel and pointer-leave,
  menus, type changes, routes). `tools/gui_preview.cpp` renders it over the
  real core holding a representative patch; scenarios `rest`, `type-menu`,
  `route-menu`, `lfo`.
- `host-smoke` drives `Voice Drift` as the plain (latency-free) parameter.
  `tools/clap_host_probe.cpp` no longer reads an output-meter parameter
  (Orrery has none; the editor gets telemetry from the bridge).
- The fixed CLAP table includes fields for all possible slot types. Host writes
  to inactive fields are ignored, so their reported values remain patch
  defaults and survive a state round trip. Active wave choices are limited to
  the oscillator type; unsupported choices resolve to that type's default.
  Odd phaser stage counts resolve to the next even count. Empty step sequences
  omit `values` from state, which the patch codec reads as the default.
- The GUI library is compiled as C++20 (the core's headers need it) and links
  `supple::orrery-clap`.

## Changes useful to the template

- `test/host_contract.h`: a parameter whose minimum displays as `-inf …` (an
  "off" level) may accept the text `-inf`, provided it decodes to exactly the
  minimum. Everything else must still reject non-finite text.
- The core supports 64-bit hosts and a mono output configuration (the stereo
  render folded to (L+R)/2) — both found by `host-smoke`.
