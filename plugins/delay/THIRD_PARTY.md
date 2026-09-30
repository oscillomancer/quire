# Third-party components

The project fetches source dependencies at configure time. Their pinned
revisions are recorded in the CMake files. Preserve the upstream notices in
source and include the applicable licenses with distributed binaries/source.

- CLAP SDK — MIT, copyright Alexandre Bique and contributors.
- clap-wrapper — MIT, copyright defiantnerd and contributors.
- Pugl — ISC-style license, copyright David Robillard.
- NanoVG — zlib license, copyright Mikko Mononen.
- nanovg_metal — MIT, copyright Ollix and contributors; the build uses the
  maintained wjakob fork pinned in `plugin/gui/CMakeLists.txt`.
- IBM Plex fonts — SIL Open Font License 1.1. The complete notice is shipped at
  `plugin/gui/fonts/LICENSE-IBM-Plex.txt`.
- Inter fonts — SIL Open Font License 1.1, copyright The Inter Project Authors.
  The complete notice is shipped at `plugin/gui/fonts/LICENSE-Inter.txt`.
- gulrak/filesystem — MIT, copyright Steffen Schümann; pulled in transitively by
  clap-wrapper.
- VST3 SDK — MIT, copyright Steinberg Media Technologies GmbH. Note that this
  applies to v3.8.0, the version clap-wrapper pins here; earlier releases were
  dual GPLv3/proprietary, which is a materially different obligation. The
  separately-licensed vstgui4 component is not used.
- Apple AudioUnit SDK — Apache-2.0, copyright Apple Inc.

The full text of every license above is reproduced in `NOTICES.txt`, which is
what satisfies the attribution requirement — naming a license is not the same as
including it. Regenerate that file whenever a pinned dependency moves.

This summary is not a substitute for the full upstream license texts or legal
review.

The template itself is MIT licensed (`LICENSE`); every component above is
compatible with that. `plugin/gui/pugl_metal.{h,m}` are derived in part from
Pugl and keep its ISC notice. Products built from the template choose their
own license; see the product identity checklist in `AGENTS.md`.

Note that the vendored Objective-C sources (Pugl and nanovg_metal) are compiled
with their class names prefixed per product -- see `plugin/gui/CMakeLists.txt`.
The sources themselves are unmodified.
