# ADR-0042: Native MIDI input

**Status:** Accepted, 2026-10-15. Amends ADR-0016 and ADR-0038; uses ADR-0027.
Contract: engine.md, native MIDI inputs.

## Decision

PortAudio opens up to 16 native MIDI sources when BackendConfig.midiInputs is
true (engine.start enables it). macOS uses CoreMIDI's protocol-1 input port
and MIDI 1.0 channel-voice UMPs. Windows uses WinMM CALLBACK_FUNCTION, with
no Windows MIDI Services requirement. Linux retains JACK; no native device
library is built there. Display/WinMM device names are fixed while running;
engine.inputs returns them, and existing device-substring/channel routing,
monitoring, learn and recording consume Backend::midiInput unchanged.

One preallocated SPSC queue (2048 messages) per source bridges the OS receive
thread to audio. Receive callbacks only decode, timestamp, copy and count drops;
no allocation, locks, waits, synchronous logging or multimedia lifecycle calls.
CoreMIDI supplies its client's receive thread; WinMM serializes input for each
opened device. Audio snapshots each queue's length, drains bounded work, maps
and stably sorts at most 1024 messages per device audio period. Overflow,
malformed/system messages and unsupported SysEx packets/buffers count toward
engine.status.midi_dropped (native backend; other backends default to zero).
Counts are discarded messages/packets/buffers, not SysEx bytes. WinMM's two
fixed 256-byte long-message buffers are recycled by the existing device monitor
rather than calling multimedia APIs from the receive callback; while both are
awaiting recycle the driver may discard further SysEx without reporting it.

CoreMIDI mach host timestamps convert with a setup-time timebase. WinMM
milliseconds since midiInStart are anchored immediately before that call with
QPC; current QPC selects the nearest 32-bit timestamp-wrap epoch. MIDI and
audio use the same source clock. At audio callback entry the preceding period
[end - frames/rate, end] maps to frames [0, frames-1]. This causally captures
already received events with within-period spacing; late/future timestamps
clamp to endpoints. It can add up to one device period of MIDI delivery delay;
it does not claim hardware timestamp/ADC-clock synchronization. WinMM accuracy
is limited to its millisecond timestamps and start-call anchoring. Engine
sub-blocks rebase those period offsets, including a final short chunk; an event
is delivered exactly once. No changes to offline/timeline MIDI or recording's
existing capture-latency compensation.

Opening is transactional: failure closes MIDI and audio resources and reports
backend.midi at /midi_inputs with a reconnect/retry hint. No sources is success.
Native topology changes (source count/order/identity, WinMM count/caps/name)
are observed on the 250 ms device monitor and wake the existing owner restart
path with reason "MIDI devices changed". No live port/name mutation. Identical
WinMM devices with indistinguishable caps/names cannot be distinguished across
a replacement without an observed topology change. Stop joins the monitor and
audio callback, resets/disposes MIDI inputs and their callbacks, then destroys
queue storage. MIDI queues do not wake the model: the next running audio cycle
feeds the engine's existing control relay, monitoring and recording paths.

## Validation

An injectable MidiSource uses the exact production queue/timestamp/chunking
path with the fake PortAudio driver on Linux and native systems. Tests cover
clamping, sorting, chunk edges, loss, timestamp wrap, transactional startup,
concurrent per-port producers, restart notification, filtered/rechannelled live
notes, recording positions and learn-ring input in both engine executors.
Existing fake-device/offline bit identity tests remain. Callback tests run under
RTSan and concurrency/lifecycle tests under TSan. Native CI verifies SDK linkage;
SUPPLE_MIDI_DEVICE_TEST=1 alone enables the hardware MIDI test with fake audio.
It opens/enumerates/briefly services MIDI inputs; no physical performance or
hardware timing accuracy is claimed by device-free tests.

References: [CoreMIDI receive block](https://developer.apple.com/documentation/coremidi/midireceiveblock),
[WinMM timestamps](https://learn.microsoft.com/en-us/windows/win32/multimedia/mim-data),
[callback restrictions](https://learn.microsoft.com/en-us/previous-versions/dd798460(v=vs.85)),
[reset/close](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinclose).
