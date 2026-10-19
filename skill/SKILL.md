---
name: quire
description: Operate the Quire DAW and its stock plugins (synth, EQ, dynamics, effects) through their MCP tools — write MIDI in Quire notation, program synth patches, render and analyze audio, mix, route, render stems. Use whenever a task involves making or changing music, sounds or mixes in Quire or its plugin sandbox.
---

# Quire

Quire is a DAW whose every action is a typed command. You drive it through
MCP tools (in the sandbox: `plugin_*`, `patch_*`, `render`, `analyze`, …; in
the DAW: a dozen tools plus `describe` / `exec` / `query` for the rest).
Humans use the same commands through the GUI, so anything you do shows up in
their undo history and can be reviewed.

## Rules that matter more than anything below

1. **Measure, don't guess.** You cannot hear. After every change that
   affects sound, render and read the analysis report (`render` with
   `"analyze": "summary"`, or `analyze`). Compare against a reference with
   `compare`. Numbers beat intuition.
2. **Use real units.** Values are physical: `"cutoff": "1.2 kHz"`,
   `"attack": "5 ms"`, `"gain": "-3 dB"`, `"fine": "+7 ct"`. Plain numbers
   are in the parameter's canonical unit (Hz, ms, dB, st, ct, %).
3. **Change small things with merge patches.** `patch_merge` with only the
   members you want to change; `null` deletes a member. Read the current
   patch first (`patch_get` with `"detail": "changed"` is short).
4. **Group related steps into one `batch`** — atomic (all or nothing), one
   undo step, and later steps can reference earlier results with
   `"${name.field}"`.
5. **Read errors; they tell you the fix.** Every error has a `path` and a
   `hint` (valid range, closest valid name, an example). Fix exactly what it
   says and retry once; don't guess around it.
6. **Taste is the human's.** Descriptors say whether you hit a target, not
   whether it sounds good. When a result matters, say what you aimed for,
   what the numbers show, and ask the human to listen. In the DAW, propose
   changes on a branch so they can A/B and accept or reject.
7. **Let the optimizer tune numbers.** You choose the structure (which
   modules, which routes, which parameters matter); `optimize_start` tunes
   up to 32 continuous parameters against a reference or a target profile.
   Don't hand-tweak ten knobs one at a time.

## Where to read next (load only what the task needs)

| Task | Read |
|---|---|
| Writing melodies, chords, drums, bass lines, controller moves | `guides/notation.md` |
| The sandbox loop: load a plugin, program it, render, analyze, iterate | `guides/sandbox-loop.md` |
| EQ: rumble, masking, harshness, de-essing, dynamic and mid/side bands (Yarrow) | `guides/eq.md` |
| Early reflections, room/hall tails, held ambience (Nave/reverb) | `guides/reverb.md` |
| Designing a synth sound (Orrery) | `guides/sound-design.md`, then `plugin_describe` / `fx_describe` for exact fields |
| Corrective mixing (levels, rumble, masking, loudness targets) and automation | `guides/mixing.md` |
| Compressing (six characters, upward too), external-key ducking, catching peaks (Sundew/compressor) | `guides/comp.md` |
| Limiting the master to a true-peak ceiling, loudness targets (LUFS), level-matched comparison (Treeline/limiter) | `guides/limiter.md` |
| Gain staging, balance, width, polarity, bass mono (Plumb/utility) | `guides/utility.md` |
| Cleaning spill or noise with a gate or expander (Sluice/gate) | `guides/gate.md` |
| Saturation: drive, asymmetry, wavefolding (Kiln/drive) | `guides/drive.md` |
| Onset punch, softer attacks, shorter or longer decays (Flint/transient) | `guides/transient.md` |
| Writing a Lua script (loops, conditionals, subscriptions, one transaction) | `guides/lua.md`, `generated/engine.lua` |
| Writing a plugin of your own: an effect or an instrument as a DSP script (`dsp_write`, then `fx_add`) | `guides/dsp-workflow.md`, `guides/dsp-language.md`, `generated/dsp-examples.md` |
| Exact command arguments and results | `generated/sandbox-commands.md` (sandbox), `generated/engine-commands.md` (DAW), or the `describe` tool |
| A stock plugin's parameters and patch format | its guide, and `plugin_describe` (sandbox) or `fx_describe` (DAW) for every field, unit and range |
| Tempo-synced echoes, ping-pong, freeze and wet ducking (Echelon/delay) | `guides/delay.md` |
| Live loudness, spectrum and stereo monitoring (Gauge/analyzer) | `guides/analyzer.md` |
| What the analysis numbers mean | `guides/analysis.md` |

Files under `generated/` are produced from the code and always match the
installed version. If a guide and a generated reference disagree, the
generated reference is right.
