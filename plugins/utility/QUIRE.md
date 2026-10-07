# Quire integration notes (utility)

Template copy, MIT license retained per ADR-0011. Functional identity:
Utility / utility / UTILITY; CLAP `com.suppleaudio.utility`, state magic
`QUT1`, AU subtype `QUtl`, effect `aufx`. The display name is defined once
in `cmake/Identity.cmake`; the engine receives `SUPPLE_UTILITY_DISPLAY_NAME`.

Product changes:
- The SDK-free engine is `supple::utility-engine`, built in the monorepo
  and the isolated template project with identical sources and sanitizer flags.
  No shared library or template source was modified. The LR4 side filter,
  finite ramps and DC blocker live in this engine.
- ParamStore, ClapParams and FlatSemantic own normalized host values,
  transactional canonical JSON state and semantic patches; no legacy states
  exist. Audio adopts a whole physical-value snapshot. Describe forwards to
  the engine's signal-flow prose. Successful semantic/state changes request
  a host parameter value rescan.
- Mono/stereo and float/double processing preserve the template's port and
  sample-offset automation structure. Latency is always zero.
- Scalar peak/correlation accumulation is a small bounded audio-path operation;
  host readonly meters update every 50 ms even without the editor. Ring frames
  are produced only while the editor exists and is shown.
- Compact 520 × 300 editor with gain/pan knobs, width slider, channel selector,
  switches, bass-mono control and telemetry. It reuses the template editor's
  balanced drag, keyboard, wheel and focus-loss gesture handling and Orrery's
  palette. Values sit below the dials; pan uses a centre-origin bipolar arc.
  Header branding and title have separate lines. Preview scenarios: default,
  wide, pan-left, pan-right.
- All copied C++ uses the vendor namespace `supple::utility`. The window,
  renderer, widgets, rings, entry and probes otherwise keep template code.
  Frame adds correlation; host_contract carries Orrery's accepted off-at-min
  text rule. CMake follows ProbeSynth's offline monorepo seam and system
  NanoVG includes. Obsolete template core tests and CLI were removed.

Spec decisions are recorded in stock-effects.md §8 and the utility skill guide.
