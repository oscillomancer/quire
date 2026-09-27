# ADR-0004: Audio backend strategy

**Status:** Accepted, 2026-09-26. Evidence: `docs/research/linux-audio.md`.

## Context

Target: Arch, PipeWire 1.6.9 with pipewire-jack (no jack2), WirePlumber
0.5.17, MOTU M4 interface. The survey found `/usr/lib/libjack.so` is
pipewire-jack's MIT-licensed implementation, so plain JACK clients work with
no `pw-jack` wrapper; a probe client ran at 1024 frames / 48 kHz with zero
xruns. The kernel is `7.2.7-zen1` `PREEMPT_DYNAMIC` (not the PREEMPT_RT
kernel the prompt assumed); `ulimit -r` is 95 and rtkit is running, so RT
scheduling is available to the user. The graph quantum defaults to 1024 (min
32, max 2048), so 64-sample operation must be requested per client.

## Decision

1. **Backend interface.** The engine talks to an `AudioBackend` interface
   (open/close with a requested config, start/stop, one RT process callback
   with sample-accurate MIDI in/out, port registration, reported
   input/output latency, xrun counting, and notifications for buffer-size,
   sample-rate, device and port changes — delivered to the control thread,
   never handled on the RT thread). Platform code stays behind it.
2. **JACK API first** (via pipewire-jack here, jack2 elsewhere). `libjack`
   is **dlopen'ed** through a small symbol table of our own, so binaries start
   and fall back cleanly on systems without it. Buffer size is requested with
   `jack_set_buffer_size` and/or the client-scoped `PIPEWIRE_QUANTUM`
   environment variable (a per-process request, not system configuration);
   whatever the graph grants is honoured and reported. MIDI uses JACK MIDI
   ports (PipeWire bridges hardware MIDI).
3. **Native PipeWire second** (`pw_filter`), once the engine exists, if
   JACK-on-PipeWire shows limits in device control, latency reporting or
   quantum negotiation. Also dlopen'ed.
4. **ALSA direct as fallback** (alsa-lib is an OS library, dynamically
   linked), for systems with neither server.
5. **Dummy backend** (a timer-driven thread that calls the process callback
   at the configured rate) and **offline backend** (the engine's own render
   loop, faster than real time) for tests, CI and rendering. Offline render
   never goes through JACK freewheel, so it is deterministic and independent
   of the audio server.
6. The engine's RT thread is the backend's callback thread. The engine
   never opens the device at build or test time unless a test requires it,
   and releases it afterwards (the machine is Nate's workstation).

## Consequences

The Phase 2 sandbox needs no audio device at all (offline only). Phase 4
starts with JACK + dummy + offline backends. Benchmarks at 64 frames need
the client-side quantum request and a check that the graph actually runs at
that size (other clients may force a larger quantum; this is reported, not
"fixed").

## Alternatives considered

Native PipeWire first (best integration, but a larger API, no ecosystem
portability and the JACK API already works here); ALSA first (bypasses the
desktop graph and competes for the MOTU); linking libjack directly (simpler,
but a hard runtime dependency).

## Amendment (2026-09-26, implementation)

pipewire-jack reads `PIPEWIRE_LATENCY` as `node.latency` (a request) and
`PIPEWIRE_QUANTUM` as `node.force-quantum` + `node.force-rate` (forcing the
whole graph, including its rate). The backend therefore requests the block
with `PIPEWIRE_LATENCY` and keeps `jack_set_buffer_size` behind an explicit
option for benchmarks. First device run: a 64-frame request was granted a
128-frame period at 48 kHz with zero xruns over 2 s; the 64-frame acceptance
benchmark will need the forced period (reported with the results).

