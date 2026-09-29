# Analyzer integration

The display name is defined once as `supple::analyzer::kName`; the directory,
CLAP id (`com.suppleaudio.analyzer`), symbols, AU subtype (`QAna`) and artifact
(`Analyzer.clap`) use the functional identity. State magic is `QAN1`.

Copied the verified effect template without changing it. The MIT license is
retained under ADR-0011. The window, renderers, widgets, Ring implementation,
entry points, fonts, wrapper patches and host contract remain byte-identical
apart from identity renames. The product smoke harness replaces the shared gain-render helper (which requires
non-identity DSP) with exact float/double pass-through checks at five rates and
four block sizes, in-place and separate. The shared contract header is unchanged.
The host probe's single meter lookup names our
true-peak id rather than the template's output meter.

Product-owned changes:

- The SDK-free `engine` replaces the placeholder DSP and parameter table.
  Audio is copied bit-exactly in mono/stereo, float/double, with zero latency.
  A preallocated SPSC packet queue carries measurement samples and control
  changes. A joined worker consumes it; no measurement, FFT, JSON, allocation,
  wait or thread creation happens in process. NaN/Inf and float overflow are
  sanitized only in the measurement copy, never in the output.
- `core-analysis::LiveMeter` reuses `LoudnessMeter` and adds rolling stereo
  sums and a Hann FFT. `LoudnessMeter` now supports bounded rolling horizons;
  its existing since-reset behavior and exact-mode offline callers are kept.
  Finite windows expire on the 100 ms observation grid. The spectrum averages
  power with a 300 ms time constant. Correlation, width and balance use the
  same definitions as offline analysis.
- Window changes begin fresh history; hold excludes incoming measurement
  samples until resumed; reset clears history even while held. Queue overflow
  drops new measurement packets, counts the lost frames and resets history
  on the next accepted packet. Audio output stays unchanged.
- CLAP params/text/state/patches use ParamStore, ClapParams and FlatSemantic.
  Readonly scalar events are emitted by the audio thread at most every 50 ms.
  The main thread formats `get_telemetry` JSON from a wait-free atomic snapshot.
  The extension ABI appends this optional member; other plugins expose null.
  The host's original-member size check uses the appended member's offset,
  preserving old binaries, and its fixture advertises the original size.
- The editor presents M/S/I, LRA, TP, short-term history, the spectrum and
  stereo meters, with window/hold/reset controls. Theme tokens match Orrery.
  Keyboard and wheel gestures remain balanced. Editor telemetry is generated
  only when visible, including native fallback-timer hosts.
- The isolated CMake build reuses the established monorepo integration and
  vendored dependencies. C++20 and sanitizer flags cover product targets;
  template infrastructure is excluded from the format gate. The redundant
  template gain renderer is removed; core, editor and CLAP tests cover the
  actual analyzer. `gui_preview` feeds synthetic music through the real engine.

Shared changes worth offering upstream: a measurement worker/queue seam and
an optional semantic telemetry callback. No upstream template edits were made.
