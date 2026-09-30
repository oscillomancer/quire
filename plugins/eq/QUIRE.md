# Quire integration deviations

Instantiated Nate's MIT template, without changing its source repository. Identity
renames are Eq / eq / EQ, CLAP `com.suppleaudio.eq`, state magic `QEQ1`, AU subtype
`QEq_`. The display name is configured once in `identity.cmake`; CMake artifacts,
the descriptor and editor read the generated identity. The source license stays
MIT under ADR-0011, overriding the template's product-license recommendation.

The SDK-free `engine` replaces the template identity DSP. Its permanent table
has 59 parameters. The format layer forwards physical values, normalized host
conversions, canonical JSON patches and transactional framed state to
`ParamStore`, `ClapParams`, `FlatSemantic` and the engine. A custom description
thunk supplies the engine's signal-flow prose. No legacy state exists for this
new plugin. Phase changes request a host restart; the active phase and latency
stay fixed until reactivation. A dedicated host regression checks this contract.

Minimum phase uses TPT SVFs and Butterworth cascades through order 16. Natural
phase uses a local copy of Orrery's original permissively licensed half-band
coefficient design with a new interpolation path and four to seven 2× stages (16×…128×).
Linear phase uses original radix-2 FFT and 128-sample partitioned convolution;
kernels compile off the audio thread, publish through RtHandoff and interpolate
over 20 ms on the internal partition grid. A worker exists only while linear
mode is active and joins on deactivate/destruction. FIR updates are coalesced;
minimum/natural events apply at their host sample offsets. All storage is sized
at prepare; no library primitive or shared plugin code was modified.

The editor owns its response graph, eight band columns, output controls, menus,
keyboard interaction and Orrery-family palette. Audio only copies stereo pre/post
samples into the unchanged SPSC ring; the editor batches 4096 samples and uses
core-analysis spectral-only analysis for LTAS. The ring itself, window, renderer,
widgets, wrapper entry and host contract retain template infrastructure
apart from identity. The GUI probe measures output sample peaks because this EQ
has no meter parameters. The wrapper supports concurrent frequency/gain gestures by
reserving two final boundary slots per gesture.

Build integration follows ProbeSynth: C++20, vendored dependency overrides,
read-only wrapper-patch checks, the local-library CMake seam, CLAP by default,
optional VST3, ExternalProject isolation, and product-owned format-gate entries.
The plugin links core-analysis for editor spectrum analysis in its isolated build.
The template CLI was omitted because its identity-effect switches do not describe
this EQ; engine tests exercise the same processor offline.

Preview usage: `eq-gui-preview 0 out.ppm 1 flat` (or `mix`); convert the capture
with `magick out.ppm out.png`. The mix fixture feeds synthesized audio through
the real engine and telemetry path. Host-smoke is exercised in stereo and mono;
semantic-rt additionally checks transactional state and patch replacement while
processing. These checks do not establish REAPER or macOS/Windows acceptance.
