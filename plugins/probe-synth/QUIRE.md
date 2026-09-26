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
