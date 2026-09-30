# Nave

An original stereo algorithmic reverb: early reflections, allpass diffusion,
and a 16-line Hadamard feedback network with frequency-dependent decay.
Its primary interface is a physical-unit JSON patch (`supple.reverb-patch/1.0`).

Build from the repository root with `cmake --preset dev` and
`cmake --build --preset dev -j6`. The CLAP artifact is
`build/dev/plugins/clap/Nave.clap`. See `QUIRE.md` for integration details,
`../../skill/guides/reverb.md` for controls and recipes.

## License

MIT; notices travel alongside the plugin artifact.
