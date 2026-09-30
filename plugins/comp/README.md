# Stock compressor / limiter (`comp`)

SDK-free dynamics engine with a template-derived CLAP editor and semantic
JSON patches. See [integration notes](QUIRE.md),
[the parameter guide](../../skill/guides/comp.md), and
[the authoritative spec](../../docs/specs/stock-effects.md#2-compressor--limiter--ballast-comp).

Build from the monorepo with `cmake --preset dev` and
`cmake --build --preset dev -j6`. Run `ctest --test-dir build/dev -R comp --output-on-failure`.
Code is MIT; dependencies retain their licenses and notices.
