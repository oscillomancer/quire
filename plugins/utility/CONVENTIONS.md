# Engineering conventions

This document captures the decisions that are architectural or behavioral and
therefore cannot be preserved by copying files alone. They come from the useful
common ground between the products built from this lineage -- Ferric, Plateau
and Vellum -- including the defects found while auditing them.

Where a rule below names a specific file or symbol, that name is the contract.
The code that carries a correctness rule -- `Telemetry.h`'s `Ring` and
`test/host_contract.h` -- is kept identical across products apart from the
product's own namespace and license header, so a fix made in one place is
expected to be made in all of them. Drawing code is product-owned; see "Visual
identity".

## Product and DSP boundaries

There is one DSP core. It includes no CLAP, VST3, AU, Pugl or NanoVG header and
can be rendered in a command-line test without loading a plugin. The format
layer translates host events into its parameter struct; it does not contain a
second version of the algorithm. All formats therefore differ only in wrapper
behavior, never in sound.

The core owns signal history, delay lines, smoothers and preallocated scratch.
`prepare()` establishes sample-rate/channel topology and allocation. `setParams()`
updates bounded targets. `process()` performs steady-state audio work. Reset and
re-prepare preserve the current parameter state and do not revive constructor
defaults.

Prefer physically or mathematically meaningful state to arbitrary waveshaping
when a product claims modeling behavior. Listening decides taste, while tests
establish factual properties: latency, bandwidth, aliasing, ceiling behavior,
block invariance, nulls, channel behavior, CPU cost and denormal safety. Loudness
comparisons should attenuate the louder render rather than boost the quieter one.

## Real-time discipline

The normal audio callback is bounded and allocation-free. No mutex, system call,
file/network I/O, exception, logging, GUI access or unbounded retry belongs in
it. Vectors and delay buffers are sized during activation. Avoid saying the
whole path is allocation-free unless an allocator-instrumented test proves it;
source inspection only proves the narrower hot paths inspected.

Move expensive parameter-derived geometry, filters or kernels to a controlled
update cadence and coalesce dense automation where the tiny control latency is
acceptable. The final value must remain exact. Do not apply this pattern to an
audible sample-accurate control without measuring the consequence.

Treat silence, NaN/Inf input, denormals, sample rates from 44.1 through 192 kHz,
host blocks from 1 through thousands of samples, float/double buffers, mono,
stereo and declared multichannel layouts as normal test cases.

## Parameters, automation, state and latency

`UtilityParams.h` is the source of truth for IDs, names, modules, ranges,
defaults, units, flags, normalization and text. Generic host UI, custom editor,
automation and state must agree with it.

CLAP events are applied at their sample offsets by splitting only at actual
events. UI changes are mirrored locally for immediate feedback and sent through
a bounded queue to the host. The stopped-transport path uses `params.flush`;
playback also drains edits in `process`.

A gesture is all-or-nothing. A BEGIN is only published when the queue can also
hold the exact final value and the END, because a BEGIN whose END is dropped
leaves the host with a parameter latched in "being edited" forever. When the
queue cannot promise those slots -- a stopped or unresponsive host is the usual
cause -- the whole gesture is deferred as one `ParamGesture` and expanded into
its three events later. Intermediate values may be coalesced away; the final
value may not. Drain with `peek`/`consume` so an event the host's output queue
refuses is retried rather than lost, and ask for the retry through
`host->request_callback` -- `request_flush` is forbidden on the audio thread.

State is a versioned list of parameter ID/value pairs. New parameters append;
missing old values receive current defaults. Unknown retired/future IDs are
ignored safely. Never serialize a C++ struct or depend on display order.

Latency is a protocol contract, not only an integer. If a future parameter can
change it, document whether it requires stopped transport or plugin restart and
implement the correct CLAP callback/restart behavior. Keep bypass latency
compensated when the processor itself has latency, unless the product explicitly
chooses a different host-visible behavior.

## Telemetry and metering

The UI never reads mutable DSP objects directly. Audio publishes small plain
frames through a two-index SPSC ring; GUI consumes them. Producer and consumer
each own one index, publication uses release/acquire ordering, and a full queue
drops the newest frame instead of overwriting a slot being copied.

`Ring<T, CapacityPow2>` in `Telemetry.h` is that queue, and it is shared code:
identical in every product apart from the namespace. Its API is the contract --
`push` returns false rather than overwriting, `peek`/`consume` let a consumer
look at an item before committing to having taken it, and `free()` reports the
remaining slots. `pop` is `peek` plus `consume`. The payload structs around it
(`Frame`, and whatever else a product needs) are the part that is product
specific; the queue itself is not.

Editor visibility gates telemetry work. The utility uses 250 Hz observations,
which is responsive without turning one host block into hundreds of tiny DSP
calls. Choose a rate from what the display needs, not from the audio sample
rate: Vellum runs at 1 kHz because a two-millisecond drum attack has to be
several columns wide, and that is a display requirement rather than a default
to copy.
Where possible capture a raw meaningful state at control rate rather than run a
second expensive model solely for the graph.

Meter labels must match the measured point and definition: sample peak is not
true peak, saturation including bias is not audio-only saturation, linear tape
loss is not complete nonlinear response, and gain reduction must not include
unrelated linear filtering. If no honest compact label exists, explain it in
the UI help/documentation.

## GUI architecture

The panel renders in one fixed design coordinate space. `Window` performs the
single transform to the host view and handles backing scale. Resize keeps aspect
ratio and varies scale rather than stretching layout. Editor hit testing uses
the same design geometry and works before the first expose event.

Pugl owns the embedded native view and events. NanoVG owns product drawing.
macOS uses the local Pugl Metal backend plus `nanovg_metal`; Linux and Windows
use NanoVG GL2. A separate GL editor is built on macOS only for framebuffer
capture by the preview tool. Renderer code remains behind a tiny C seam so the
product UI and input logic contain no renderer-specific branches.

The host timer drives roughly 60 Hz ticks. clap-wrapper's AUv2 timer registration
can refuse the CLAP timer request, so the realized native view starts a Pugl
fallback timer on macOS/Windows. X11 Pugl timers themselves require `puglUpdate`,
so a Linux host that refuses CLAP timer support remains an explicit integration
assumption to test in real Linux hosts.

Repaint only when telemetry, automation, input, resize or an unfinished timed
animation makes the image dirty. Peak holds, decays and other motion use actual
elapsed time between draws, so CLAP/VST3/AU hosts with different refresh rates
show the same dynamics.

Keyboard support is part of every product: Tab/Shift-Tab focus, arrows, Shift
fine adjustment, Home/End bounds, Enter/Space toggles, visible focus, and focus
loss that closes every gesture. Text contrast must remain legible without
flattening the product's visual identity.

## Platform and release priorities

CLAP on Linux is first-class. macOS must build CLAP, VST3 and AUv2 from the same
implementation; Windows builds CLAP and VST3. Platform support is evidence-based:
a successful macOS compiler run does not verify X11 embedding, Windows GL symbol
loading or behavior in a Linux DAW.

Every dependency in the stack requires its notice to accompany what is
distributed, and that obligation is independent of the licence the product
itself ships under. `NOTICES.txt` reproduces those texts in full -- naming a
licence is not the same as including it -- and the build stages it into each
bundle's `Contents/Resources`, because a recipient handed only a binary never
sees the repository. That copy is an always-out-of-date target rather than a
`POST_BUILD` on the plugin targets: a `POST_BUILD` runs only when its target
relinks, so editing the notices alone would ship the previous copy. The AU's
copy is made inside the prepare step instead, before the code signature, since
adding a file to a signed bundle invalidates it. Regenerate `NOTICES.txt`
whenever a pinned dependency moves.

Keep the dependency commits pinned -- an explicit commit, never a branch name:
a moving `GIT_TAG main` means two checkouts of the same source can produce
different plugins. Revalidate all formats before updating any SDK or GUI
dependency. In particular, do not alter the opposite Objective-C ownership
requirements: Pugl/custom Pugl Metal/backend glue are non-ARC, while
`nanovg_mtl.m` is ARC.

`CMAKE_OSX_DEPLOYMENT_TARGET` is set before `project()` and with `FORCE`.
`project()` creates the cache entry, so a later plain `set()` is silently
ignored and the deployment target is whatever the toolchain defaulted to.

Warnings are on for the project's own targets and off for vendored ones, via
the `<product>_target_defaults()` helper: third-party sources are not ours to
keep clean, and burying our own warnings in theirs is how they get ignored.
The same helper carries the ASan/UBSan switch, exposed as the `sanitizers`
preset in `CMakePresets.json`.

Every product retains these baseline checks, plus product-specific regressions.
A new product adds coverage rather than dropping applicable checks:

- `core` -- the DSP, offline and SDK-free.
- `editor-input` -- gesture and keyboard handling, no renderer.
- `wrapper-automation` -- that the clap-wrapper VST3 automation patch is still
  applied to the pinned checkout.
- `auv2-bundle` -- that the built AU has clap-wrapper's AudioComponents plist
  and a valid signature, rather than CMake's generic one that no host reads.
- `host-smoke` -- a real host that dlopens the built `.clap` and drives the
  full lifecycle: parameters, processing, latency/restart behaviour, state
  round trip, teardown. Shared port/text/render assertions live in the
  byte-identical `test/host_contract.h`; `host_smoke.cpp` retains product-specific
  lifecycle and state regressions. Product identity is supplied as compile
  definitions (`HOST_SMOKE_PLUGIN_ID`, `HOST_SMOKE_PLAIN_PARAM`, and optionally
  `HOST_SMOKE_LATENCY_PARAM`). The harness
  detects an instrument from a declared audio-input count of zero rather than
  being told, and it searches a latency-changing parameter's range for a
  setting that actually moves latency rather than assuming which one does.

An install target moves every built format together so a DAW cannot accidentally
load a stale CLAP beside a new AU. Ordinary builds never write into user plugin
folders. Release validation should include clean configure/build, unit tests,
CLAP bundle probe, resize/reopen/hot teardown/fallback timer, format scanners,
DAW automation/state recall, sample-rate matrix and a final artifact identity
check.

## Visual identity

Infrastructure should remain consistent; products should not all look like a
reskinned template. Keep the interaction language, typography quality, spacing
discipline, accessibility and renderer architecture, while deriving layout,
telemetry centerpiece, palette and visual metaphor from what the DSP actually
does. A graph earns its space only when it explains meaningful product state.

`Telemetry.h`'s `Ring` and `test/host_contract.h` are shared code and stay
identical apart from namespace; a fix to either is made in every product.
`Widgets.h` is product-owned: it is where a product's look lives, and products
are expected to differ there. What is shared is the contract around it: every
`Theme.h` defines the token set below and gives it product values, controls
honour the interaction rules in "GUI architecture", and the lessons recorded in
each product's AGENTS.md about text, glyph coverage and hit testing apply
everywhere.

- `ink`, `panel`, `well`, `edge`, `edgeHi` -- ground, raised surface, recess,
  hairline, lit hairline. `well` is the inside of a recess, not a knob.
- `dim`, `dust`, `bone` -- text, from least to most emphasis.
- `accent`, `alert` -- the product's accent, and over/error.
- `knobWell`, `knobTop`, `knobBot`, `knobRim` -- the knob's track, its body
  gradient, and its ring. Required because a knob needs four tones and must
  not hard-code them; a knob drawn in another product's greys is the bug this
  prevents.

A product adds whatever else it needs beyond that. Fonts are registered under
product-prefixed names (`<product>-mono`, `-mono-medium`, `-display`, `-ui`,
`-ui-semibold`). Inter UI faces draw labels as complete text runs; Plex Mono
is reserved for numeric data and Plex Sans Condensed remains available for
display type. The widgets use those names from `Theme.h`; do not
duplicate font selections in a product editor. The Inter glyph subset includes
Latin-1, common punctuation and the true minus sign; check coverage before
introducing a new symbol. `rgb`/`rgba` are the same two helpers everywhere --
`rgba` keeps alpha as a float rather than rounding it through a byte.

Within a product, do not grow a local copy of a widget to get a variant of it;
add the variant to that product's `Widgets.h`.
