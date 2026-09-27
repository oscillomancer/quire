# B1 (preliminary): engine capacity, anticipative vs in the callback

**Date:** 2026-09-27. **Tool:** `host/engine/bench/engine_bench.cpp`
(`build/release/host/engine/engine-bench build/release/plugins/orrery/orrery-headless.clap
--tracks … --seconds … --block 64`). **Build:** release preset at faf197c.

Preliminary because B1 (engine.md §7) calls for Orrery + stock EQ + stock
compressor per track and a monitored audio track, on the audio device; the
stock EQ/compressor do not exist yet and no device was used. Here each track
is one Orrery instance with the synth.md §9 reference patch holding an
8-note chord (3-voice unison, 24 voices), and a paced thread stands in for
the device at 48 kHz / 64 frames.

**Machine (read-only facts):** Intel Core Ultra 5 225F, 10 cores (1 thread
per core), Linux 7.2.7-zen1, cpufreq governor `powersave`, 30 GiB RAM.
Worker pool: 7 helpers + the renderer (every core but two).

| tracks | offline × real time, 1 thread | offline × real time, pool | in the callback: late cycles | anticipative: misses |
|---:|---:|---:|---:|---:|
| 2 | 4.22 | 7.08 | 0 / 3749 | 0 / 3750 |
| 4 | 2.11 | 6.70 | 1 / 3749 | 0 / 3750 |
| 8 | 1.08 | 6.28 | 99 / 3749 | 0 / 3750 |
| 12 | 0.70 | 4.20 | 2683 / 2683 | 0 / 3750 |
| 16 | 0.54 | 3.11 | all | 0 / 3750 |
| 32 | 0.27 | 1.61 | all | 0 / 3750 |
| 48 | 0.18 | 1.21 | all | 0 / 3750 |
| 64 | 0.13 | 0.92 | all | 0 / 11250 (15 s) |
| 80 | 0.11 | 0.76 | all | 0 / 11250 (15 s) |
| 96 | 0.09 | 0.64 | all | 0 / 11250 (15 s) |
| 112 | 0.08 | 0.54 | all | 1881 / 11250 (15 s) |

Reading it:

- **In the callback** (everything processed at the device block on one
  thread) the session keeps up to about **4 tracks**: at 8 some callbacks
  already overrun their 1.33 ms period, from 12 all do.
- **Anticipative** (ADR-0012: 1024-frame blocks rendered 200 ms ahead on the
  worker pool; the callback only copies) keeps up to **96 tracks** with no
  miss over 15 s and fails at 112 — about **20×** the in-callback capacity
  (the B1 target is ≥ 3×). A run passes only if the render keeps pace within
  the 200 ms buffer over its length (≥ ~99 % of real time for 15 s).
- The offline columns include creating and activating every instance, so
  they understate steady-state throughput (hence 64–96 tracks pass in real
  time while the offline pool figure is below 1).

Still to do for B1 proper: the stock EQ and compressor in each chain, one
armed track monitoring input (the live region), the audio device at a forced
64-frame period (a per-client request that affects the whole PipeWire graph
while it runs — to be scheduled with Nate), 5-minute runs, and B2 (the same
structure in REAPER).
