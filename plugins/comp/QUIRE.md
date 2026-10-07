# Quire integration notes (`comp`)

The template is copied with Comp/comp/COMP identity tokens, CLAP id
`com.suppleaudio.comp`, AU subtype `QCmp`, effect type `aufx`, state magic
`QCM1`. The working display name lives once in `cmake/Identity.cmake` and
feeds the engine constant, GUI, descriptor and artifact path. MIT is retained
under ADR-0011; the copied checklist's GPL conversion does not apply.

Product-owned changes:

- The SDK-free `engine` library replaces the template DSP header. One
  core-params table provides all physical ranges, curves, flags and text.
  Local TPT key filters and a preallocated monotonic limiter deque live
  here; shared core-dsp supplies the 4x interpolator and DenormalGuard.
  A prepared stereo key delay aligns filtered-key audition with reported latency
  and stays populated during audition/key-source transitions; process allocates
  no delay storage.
- `comp_clap.cpp` retains the template's GUI lifecycle, bounded gesture
  queues, sample-offset event splitting, mono/stereo configuration and
  float/double processing. It forwards params and JSON state to plugin-support,
  adds a non-main stereo key input, publishes readonly GR every 50 ms, and
  requests host restart for latency changes while retaining the active delay.
  The semantic description delegates to the bounded engine prose.
- This is a new identity with no shipped legacy state. Only transactional
  framed JSON state is accepted. Ratio 100 formats as infinity in the generic
  host and GUI; the table owns the value and curve.
- The 900x560 editor uses Orrery's palette and fonts, showing the transfer
  curve, live detector dot, four-second GR history, sample peak readouts,
  and detector/key/output controls. Inter is a fallback for infinity, which
  is absent from the bundled Plex Mono subsets. Telemetry is 20 Hz and only queued while
  an editor exists; the host GR parameter works with the editor closed.
  `Telemetry.h` changes only its product frame payload; Ring is preserved.
- CMake uses the existing LinkMonorepo seam and vendored dependency paths.
  The format gate excludes unchanged infrastructure. Window, Gui, widgets,
  renderer backends, host contract and probes keep template implementation
  apart from namespace, with one probe GR parameter id substitution.
  The product host-smoke harness runs the unchanged shared port assertions
  against a main-port view and separately checks the real key declaration;
  it services requested restarts before checking a new latency. This handles
  the template harness's one-input and immediate-latency assumptions.
- `semantic_rt` checks sidechain declarations, corrupt-state rejection, state
  restoration, and concurrent main-thread patch replacement/audio processing.
  Engine tests cover response, timing, keys, filters, latency, determinism,
  stress, mono/double, recovery and true peaks using core-analysis.

Preview scenarios `default`, `heavy`, `limiter` feed synthetic audio through
the actual engine and telemetry. The preview tool writes PPM; convert its
output to PNG under `build/dev` for review. Preview drawing does not establish
REAPER or other-platform host acceptance.

Review corrections: limiter release is capped by the moving-average target,
preserving a smooth lookahead attack. TP detection uses only the 4x signal,
without the L1 bound or added margin; its 16-sample detector delay is excluded
from the averaging length. New tests measure the attack ramp, a transparent
-3 dBFS sine, and +12 dB driven seeded pink noise against a -1 dBTP ceiling.
Knob values are outside the pointer face; the editor disables unused limiter
controls and plots the combined makeup/mix/output drive through the ceiling.
Linux previews use a fixed-size embedded parent for exact 900x560 captures.
