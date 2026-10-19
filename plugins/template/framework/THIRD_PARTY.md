# Third-party components

Fetched at configure time and pinned to exact commits in
`framework/cmake/Pins.cmake`. The full text of every license is in
[NOTICES.txt](NOTICES.txt), which the build stages inside every plugin bundle
(naming a license is not the same as including it). Regenerate NOTICES.txt
whenever a pinned dependency moves.

| Component | License | Notes |
| --- | --- | --- |
| CLAP SDK | MIT | 1.2.10 (newer than clap-wrapper's own pin) |
| clap-wrapper | MIT | v0.16.0, ten local patches in `cmake/patches` |
| VST3 SDK | MIT | v3.8.0 as pinned by clap-wrapper (earlier releases were GPLv3/proprietary; vstgui is not used) |
| Apple AudioUnit SDK | Apache-2.0 | macOS only, via clap-wrapper |
| gulrak/filesystem | MIT | via clap-wrapper |
| Pugl | ISC | One local Windows module/class ownership patch; `gui/pugl_metal.{h,m}` derive in part from it and keep its notice |
| NanoVG | zlib | |
| nanovg_metal (wjakob fork) | MIT | macOS only |
| nanosvg | zlib | the editor's SVGs (`gui/Images.h`); parsed only, never rasterized |
| dr_wav, dr_flac (dr_libs) | MIT-0 (or public domain) | WAV, AIFF and FLAC decoding (`dsp/AudioFile.h`) |
| IBM Plex Mono, Plex Sans Condensed | SIL OFL 1.1 | subsets in `gui/fonts`, compiled in |
| Inter | SIL OFL 1.1 | subsets in `gui/fonts`, compiled in |
| Fraunces | SIL OFL 1.1 | subsets in `gui/fonts`, compiled in |
| MTS-ESP client | 0BSD | only with `PRODUCT_TUNING ON`; its notice is added to the product's NOTICES.txt then |

The framework itself is MIT ([LICENSE](LICENSE)). A product adds notices for
its own dependencies in `product/NOTICES.txt`, which the build appends.

This summary is not legal advice.
