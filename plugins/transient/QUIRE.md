# Quire integration deviations

Flint uses template identity `Transient/transient/TRANSIENT`, CLAP id
`com.suppleaudio.transient`, AU subtype `QTrn`, state magic `QTS1`, and
version 0.1.0. Its display name is defined only in `cmake/Identity.cmake`.
The template was copied from Nate's template without modifying its source.
Template-derived code retains MIT per ADR-0011 and the monorepo rules;
the copied checklist's GPL conversion does not apply.

The SDK-free `engine/` owns the dense physical parameter table, deterministic
stereo-linked detector and gain, clip filters, and aligned dry path. It links
core-base/params/patch/dsp, never a sibling plugin engine. The parameter,
state and semantic sections of the wrapper forward to plugin_support.
State is framed canonical flat JSON, validated transactionally with no seed.
Float/double, mono/stereo and in-place audio share the same processor;
automation is applied at each event's sample offset. A bus mismatch returns
CLAP_PROCESS_ERROR rather than allocating in the callback.

Signal flow: stereo rectified maximum -> bounded cycle peak hold -> fast/slow
dB followers -> envelope-difference shaping -> 1 ms gain pole -> optional 2x
`tanh` half-band pair -> aligned dry/wet blend -> output gain. Continuous
parameters smooth for 20 ms (speed in log space); restored values snap before
the first sample. Clip is non-automatable and requests host restart; active
audio retains the old configuration and delay until reactivation. Clip off
has zero latency, on has 128 samples. There is no sidechain or bypass key;
neutral defaults pass bit-exactly. See stock-effects §4 for detector decisions.

The shared GUI lifecycle, renderer stack, telemetry Ring, host contract and
embedding probes retain the template implementation apart from namespace.
Product-owned Editor/Theme/Widgets provide the two-second envelope/gain
history, bipolar arcs and values below knobs in a 900 x 560 design. Engine
sample-peak and average-gain telemetry updates readonly host meters every
50 ms; the GUI queue is produced only when visible. Product previews embed
in a fixed-size Linux parent so tiling window managers cannot resize them.

CMake follows the existing ExternalProject offline integration: vendored
SDK/GUI sources, C++20, root sanitizer flags, CLAP by default, optional VST3
and macOS AUv2. Font assets retain their OFL notices. Useful template
adaptations include semantic JSON state, sample-offset processing,
latency-restart deferral, sanitizer coverage and the fixed-size preview parent.
