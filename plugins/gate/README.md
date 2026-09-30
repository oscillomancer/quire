# Stock gate / expander

Stereo/mono gate and downward expander with hysteresis, hold, filtered stereo
sidechain, lookahead and a latency-aligned dry/wet mix. The display name lives
once in `engine/include/gate/params.h`; code and artifact identity use `gate`.

Build: `cmake --preset dev && cmake --build --preset dev -j6` from the
monorepo. Artifact: `build/dev/plugins/clap/Sluice.clap`.

Preview: `build/dev/plugins/gate/gate-gui-preview 20 build/dev/gate-default.ppm
1 default` (or `drums`). Tests and previews need no audio device. The MIT
license and third-party notices travel beside the artifact. See `QUIRE.md`
and `../../skill/guides/gate.md` for behavior and integration decisions.

Verification commands, results and remaining acceptance: `VERIFICATION.md`.
