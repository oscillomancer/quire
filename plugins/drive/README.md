# Kiln [drive]

Stock saturation: six odd curves with first-order ADAA, 1x–8x linear-phase
oversampling, bias, a before/after tilt pair, −18 dBFS reference RMS compensation,
latency-aligned mix and output gain. CLAP with semantic JSON patches.

Build from the monorepo with `cmake --preset dev && cmake --build --preset dev -j6`.
See [QUIRE.md](QUIRE.md) for integration, [the skill guide](../../skill/guides/drive.md)
for parameters, recipes and measured limits, and `engine/` for the SDK-free core.
MIT licensed; bundled dependencies keep the notices in NOTICES.txt.
