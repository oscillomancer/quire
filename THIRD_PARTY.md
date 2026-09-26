# Third-party components

Every dependency is vendored under `third_party/<dir>/` with its license file,
and declared in `third_party/manifest.json` (machine-readable: license SPDX
expression, version, upstream, pinned commit, purpose, linkage).
`tools/license-check/check.py` runs as part of every build and fails when
anything is undeclared, not on the allow-list, missing its license file, or
not described here. Policy: [ADR-0003](docs/decisions/0003-toolchain-and-dependencies.md).

Allowed for bundled code: MIT, MIT-0, BSD-2/3-Clause, ISC, Zlib, Apache-2.0,
BSL-1.0, CC0, Unlicense (OFL-1.1 for fonts). Forbidden: GPL, AGPL. LGPL/MPL
need an approved `HUMAN_REVIEW.md` entry. Dynamically linked OS libraries
(libc, ALSA, JACK/PipeWire client libs, X11/Wayland/GL) are acceptable.

| Name | License | Version / commit | Purpose | Linkage |
|---|---|---|---|---|
| clap | MIT | 1.2.10 / `195b42a` | CLAP plugin ABI headers for hosting and for the stock plugins | headers |
| doctest | MIT | 2.5.3 / `2d0a935` | Unit-test framework; tests only, never shipped | tests |
| dr_libs | Unlicense OR MIT-0 | dr_wav 0.14.5 / `fa931f3` | WAV read/write; FLAC and MP3 decode | static (header) |
| pocketfft | BSD-3-Clause | cpp branch / `c90e55b` | FFT for offline analysis and analyzer displays | headers |

Template-derived stock plugins additionally fetch the template's own pinned
stack (CLAP SDK, clap-wrapper, Pugl, NanoVG, VST3 SDK, fonts); those are
recorded here when the first plugin lands (see `docs/research/template.md`).
