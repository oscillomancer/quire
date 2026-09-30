# Gate / expander

Sluice (`com.suppleaudio.gate`) reduces spill between drum hits and pauses,
or gently expands quiet material downward. It exposes the flat
`supple.gate-patch/1.0` patch. Use physical units in semantic merge patches;
ordinary CLAP float values are normalized curve positions. The editor and
host value text use the same parameter table.

| key | unit / values | range | default |
|---|---|---|---|
| `mode` | enum gate, expander | | gate |
| `threshold` | dB | −80..0 | −40 |
| `range` | dB attenuation floor | −80..0 | −80 (exact mute) |
| `ratio` | ratio, expander only | 1..10 | 2 |
| `hysteresis` | dB | 0..12 | 3 |
| `attack` | ms, log | 0.01..100 | 0.5 |
| `hold` | ms | 0..1000 | 20 |
| `release` | ms, log | 5..5000 | 150 |
| `lookahead` | enum off, ms1, ms5 | requires restart; not automatable | off |
| `sidechain` | enum internal, external | | internal |
| `sc_highpass` | Hz, log | 10..500, 10 off | 10 |
| `sc_lowpass` | Hz, log | 1000..20000, 20000 off | 20000 |
| `sc_listen` | bool | | false |
| `stereo_link` | percent | 0..100 | 100 |
| `mix` | percent wet | 0..100 | 100 |
| `output` | dB | −24..24 | 0 |
| `gr` | readonly dB | −80..0 | −80 |
| `open` | readonly bool | logical state of either channel | false |

The main input/output can be mono or stereo. Input port 1 is a stereo key.
Route a key to it before selecting external: missing or silent external keys
keep the gate closed. Full stereo linking uses the louder key for both main
channels; unlinked operation gates each independently. Key listen outputs
the filtered key through the same delay, mix and output controls.

Attack and release describe 63.2 % settling in the dB domain. Hysteresis puts
the closing level below threshold; hold counts from the last sample meeting
that closing level. The expander remains at unity target while latched open;
when closed, its target is `(ratio−1) × (level−threshold)`, limited by range.
That makes the hysteresis band history-dependent. `open` measures the state,
and `gr` measures gain before dry mix/output, so they need not agree visually
through an attack/release or when ratio is 1.

Three starting recipes (merge only the shown fields; listen and adjust):

1. **Drum spill:** `{"params":{"mode":"gate","threshold":-35,"range":-30,"attack":0.5,"hold":50,"release":120,"sc_highpass":160}}`.
   A finite range keeps the room audible between hits. Use key listen to
   check that cymbal bleed is not opening the gate.
2. **Quiet room expansion:** `{"params":{"mode":"expander","threshold":-50,"ratio":2,"range":-18,"hysteresis":3,"hold":40,"release":300}}`.
   Preserve low-level tails; raise threshold only after checking the source.
3. **External rhythmic key:** `{"params":{"mode":"gate","sidechain":"external","lookahead":"ms5","threshold":-30,"range":-80,"hold":30,"release":80}}`.
   Route the key first. Lookahead pre-opens for transients and needs a host
   restart. Main and dry paths remain aligned to the reported latency.

Lookahead normalizes the opening ramp to finish within the preview interval,
even for a longer attack setting; hold is at least preview + one sample.
Without lookahead, sharp transients are attenuated during attack. −80 range
settles to exact zero, but partial dry mix still passes audio. Gate/expander
processing cannot remove spill underneath wanted notes. Key filters are
12 dB/oct, clamped to 0.45 fs. The editor samples key level/gain every 50 ms
and can miss short peaks; use offline analysis or rendered audio for final
checks. These recipes are starting points, not an automatic noise classifier.
