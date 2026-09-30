# Flint — stock transient shaper

A stereo-linked, level-independent transient shaper with bipolar attack and
sustain, detector speed and sensitivity, optional 2x soft clipping, dry/wet
mix and output gain. Neutral defaults pass unchanged. The SDK-free engine,
CLAP implementation and offline WAV renderer use one DSP implementation.

See [the integration notes](QUIRE.md), [build instructions](BUILDING.md),
[the agent guide](../../skill/guides/transient.md), and
[the stock effects spec](../../docs/specs/stock-effects.md#4-transient-shaper--flint-transient).
The display name comes from `cmake/Identity.cmake`; code identity is transient.

Original and template-derived code is MIT; dependencies and fonts retain the
licenses reproduced in `NOTICES.txt` and `THIRD_PARTY.md`.
