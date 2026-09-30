# Stock EQ

An eight-band parametric equalizer with minimum, oversampled natural and linear
phase, independent stereo placement, and static pink-weighted auto gain.
The display name is defined in `identity.cmake`; the permanent CLAP identifier is
`com.suppleaudio.eq`. The parameter table and processor are in `engine/`.

Build from the monorepo with `cmake --preset dev`, then
`cmake --build --preset dev -j6`; run `ctest --test-dir build/dev -j6`.
The engine is SDK-free and covered in every sanitizer preset. See `QUIRE.md` for
integration and phase-mode limits, and `../../skill/guides/eq.md` for patches.

Original first-party code and copied template infrastructure are MIT licensed.
Vendored dependency licenses remain in the monorepo's `third_party/` directory.
