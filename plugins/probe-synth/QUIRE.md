# Quire integration deviations

## Product-specific

- Renamed all template identity tokens to ProbeSynth/probesynth, assigned CLAP
  ID `com.suppleaudio.probe-synth`, AU subtype `QPrb`, instrument type, and
  state magic `QPR1` so sessions identify this plugin uniquely.
- Replaced the identity effect with an SDK-free eight-voice oscillator,
  lowpass filter, two envelopes, seeded noise, and gain; added the 16-slot
  parameter table (15 controls and output meter), core tests, and offline note
  renderer. The plugin is an instrument with zero audio inputs, mono/stereo
  outputs and CLAP/MIDI note input. Note events are applied at sample offsets.
- Removed the template editor screenshot and replaced the effect editor's
  controls with the 15 synthesizer parameters.
  The shared window, renderer, widgets, telemetry ring, host contract and
  embedding probes retain the template implementation apart from names.
- Kept the template's MIT code license under the monorepo licensing rule and
  ADR-0011; the copied template checklist's GPL conversion does not apply.

## Changes useful to the template

- The plugin build accepts C++20 and local vendored clap-wrapper, Pugl,
  NanoVG and CLAP SDK paths, without network access. It verifies the two
  wrapper patches instead of modifying shared dependency sources.
- NanoVG is marked as a system include so its C extension warning does not
  appear in first-party C++ builds.
- CLAP is built by default; VST3 requires `PROBESYNTH_VST3=ON`. The plugin's
  ExternalProject build isolates its targets and publishes the CLAP artifact
  under the monorepo build's `plugins/clap/` directory.
- The format gate checks product-owned files and excludes the unchanged
  template infrastructure, preserving shared-file identity.
- The monorepo license checker recognizes this local first-party
  ExternalProject as code, not a downloaded dependency.

## Stage 2: semantic parameters and patch state

- `ProbeSynthParams.h` now declares one `core-params` `ParamSpec` table. Floats
  use normalized CLAP values (log cutoff and envelope times, bipolar envelope
  amount); sustain is stored in physical percent. IDs retain their Stage 1
  values. The editor reads names, units, normalized knob positions and compact
  text from this table.
- The CLAP wrapper uses `ParamStore` and `ClapParams` for values and generic
  host parameters. The audio thread calls `readAll()` when the generation
  changes, then updates the DSP from one consistent set of physical values.
  `FlatSemantic` provides canonical JSON patches, transactional CLAP state and
  the semantic extension. The binary v1 loader converts old sustain ratios to
  percent, and rejects truncated or non-finite state before applying anything.
- A patch envelope carries a seed; the processor combines it with the stable
  `osc.seed` parameter. `RtHandoff` carries an immutable validated patch snapshot (all physical
  values plus the envelope seed) to the audio thread. A new `onBeforeApply`
  hook in `plugin_support` fences the short period before that snapshot is
  published, so audio keeps the previous complete DSP state. The SDK-free
  noise source now uses `supple::Rng`.
- `plugins/common/cmake/LinkMonorepo.cmake` is the reusable ExternalProject
  include for this plugin and Orrery: with `SUPPLE_ROOT` set, it builds the
  monorepo's existing core-base, core-json, core-schema, core-params,
  core-patch and plugin-support targets inside the isolated plugin build.
  Plugin targets link `supple::plugin-support` and `supple::core-params`, so
  include paths and library dependencies follow the monorepo definitions.
  In a template-derived plugin's `CMakeLists.txt`, after `project()` and before
  defining targets, use:

  ```cmake
  include("${SUPPLE_ROOT}/plugins/common/cmake/LinkMonorepo.cmake")
  supple_plugin_add_monorepo_libraries()
  add_library(my-plugin-impl STATIC plugin/src/plugin.cpp)
  target_link_libraries(my-plugin-impl PRIVATE supple::plugin-support)
  ```

  The parent ExternalProject passes `-DSUPPLE_ROOT=<monorepo checkout>` and
  `-DSUPPLE_SANITIZE=<preset sanitizer>`; the include uses vendored dependencies
  and sets up the generated product header. A plugin may link individual core
  targets such as `supple::core-params` where its SDK-free DSP needs them.
- The plugin's CTest suite now includes semantic conformance and a real CLAP
  test that loads v1 state, checks rejection of corrupt state, and calls
  `set_patch` from the main thread while audio processes. The RT preset
  instruments the core libraries, plugin and test host.

Template changes worth offering to Nate: a reusable local-library CMake seam,
`ParamSpec`/`ParamStore` integration, the semantic extension, canonical JSON
state with transactional legacy migration, and the patch-during-processing
regression test. The shared window, renderer, host contract and probes remain
from the template copy.
