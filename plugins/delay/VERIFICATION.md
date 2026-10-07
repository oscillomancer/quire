# Echelon (delay): verification

Algorithm choices, ranges, timing, freeze and tail limits are specified in
`docs/specs/stock-effects.md` §6; `skill/guides/delay.md` describes the
patches.

## Measured DSP behaviour

The engine tests (`delay-engine.tests`) measure:

- fractional echo position, by cross-correlation, within one sample at
  22.05–384 kHz (against the same filtered reference);
- the 1 kHz repeat component within 0.2 dB of the feedback setting, and the
  loop magnitude within 0.05 dB at five sample rates;
- bit-exact output across host partitions of 1, 7, 64 and 4096 frames, with
  automation;
- a 375 → 200 ms time glide without clicks; ping-pong spacing; mono and width;
- feedback at 110 % bounded over 30 s and sustained;
- freeze RMS within 0.2 dB over 20 s, with new input rejected;
- 12 dB ducking within 0.5 dB, and its recovery;
- five seconds of automation over the full parameter domain; canonical
  restored state and deterministic wow;
- causal operation at unusual sample rates through 768 kHz (clap-validator
  once rejected 1234.57 Hz; the low-rate path and edge-rate tests came from
  that).

Drive crossfades from bypass into its curve over 0–1 dB, exact bypass at 0 and
the full curve from 1 dB. A regression starts a 0 → 2 dB ramp on a settled
0.5-amplitude echo peak: the initial change stays below 0.01 dB (it was a
0.68 dB drop before the crossfade) and successive equal-phase peaks change by
less than 0.15 dB.

## Plugin and editor checks

The plugin suite covers physical patch and state conformance, concurrent
patch replacement, gestures, mono/stereo, float/double/in-place host
contracts, transport and tail notifications, and exports. clap-validator
0.4.1 passes (36 passed, 8 skipped).

```sh
build/dev/plugins/delay/delay-gui-preview 20 default.ppm 1 default
build/dev/plugins/delay/delay-gui-preview 20 pingpong.ppm 1 pingpong
build/dev/plugins/delay/delay-linux-host-probe build/dev/plugins/clap/Echelon.clap host.ppm
```

The host probe checks embedding, mapping, resize, hide/show, three reopen
cycles, timer cleanup and concurrent audio.

Known limits (freeze period quantization, coloured-repeat measurement, the
tail estimate) are recorded in the spec and the guide.
