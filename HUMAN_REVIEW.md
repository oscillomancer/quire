# Human review queue

Things only Nate can do or decide, highest priority first. Nothing here
blocks work: each item says what proceeds meanwhile. Answer inline (edit the
item or add "**Answer:**") or tell the orchestrator; answers become ADRs or
doc updates and the item moves to *Resolved*.

## Open

### 1. Veto the project name — decision (low effort)
The DAW is called **Quire** (a gathering of vellum sheets sewn into a book;
archaic *choir*). Rationale and alternatives: ADR-0001. Renaming is cheap:
edit `cmake/Product.cmake` and rename `~/Projects/quire`.
*Meanwhile:* everything proceeds under "Quire".

### 2. Project license — decision
You asked that every line be original or permissive so you can choose any
license. Nothing is chosen yet: the repo has no LICENSE file and first-party
files carry no SPDX header (a script adds them once you choose).
Template-derived plugin files keep the template's MIT header (your code).
Options: GPL-3.0-or-later (like Ferric/Plateau/Vellum), MIT/Apache-2.0,
source-available, proprietary.
*Meanwhile:* development continues; the license gate keeps every dependency
permissive so all options stay open.

### 3. Location `~/Projects` vs `~/projects` — FYI
The prompt says `~/projects/`; this machine has `~/Projects/` (where the
template lives). The repo is `~/Projects/quire`, next to
`~/Projects/audio-plugins/`. Say so if you want it moved.

### 4. Kernel is not PREEMPT_RT — FYI
The prompt describes a CachyOS realtime kernel; the running kernel is
`7.2.7-zen1-1-zen #1 ZEN SMP PREEMPT_DYNAMIC`. RT scheduling works
(`ulimit -r` = 95, rtkit running) but you are not in the `audio` group that
`/etc/security/limits.d/99-realtime.conf` targets. Nothing is changed; the
Phase 4 xrun benchmarks will record the kernel they ran on.

### 5. OFL-1.1 fonts — FYI (policy clarification)
The prompt's allow-list is for code. Your template ships Inter and IBM Plex
(SIL OFL-1.1). ADR-0003 allows OFL-1.1 for fonts only (OFL permits bundling
with software under any license). Tell me if you want fonts treated
differently.

### 6. Name for the flagship synth — decision (low effort)
Proposed: **Orrery** (a clockwork model of orbiting bodies: modulators
orbiting a voice). Spec: `docs/specs/synth.md`. Veto or suggest another; it
is cheap to change before P3.2 starts.
*Meanwhile:* the spec and code use "Orrery".

### 7. Synth design review — decision (medium effort, when convenient)
`docs/specs/synth.md` is the draft of the synth's module set, patch format
and modulation model. You know what makes a synth good to program; a 15-minute
read now is worth more than a rewrite later. Especially: the slot pool (4 osc
/ 2 filters / 4 env / 4 LFO / 2 step / 8 macros / 5 FX), routes as patch
structure rather than CLAP parameters (macros are the automation surface),
and modulation amounts in oct/st/dB/%.
*Meanwhile:* Phase 1–2 work continues; P3.2 waits for this spec to be frozen,
with or without your comments.

### 8. Which models and budget for automated LLM evaluations — decision
The evaluation harness (`docs/specs/eval-harness.md`) runs agents against the
sandbox over MCP to measure how well they program the synth and write
notation. Runners exist for the Claude CLI, the Codex CLI, and any Anthropic /
OpenAI-compatible API (including a local llama-server). Which should it use,
and how many runs per week are acceptable cost-wise? A local model on the Arc
B580 is free but weaker; a frontier model is the real target user.
*Meanwhile:* the harness is specified (`docs/specs/eval-harness.md`) but not
built yet (corrected 2026-09-26: an earlier note here said it was); it will be
built and dry-run tested without calling models before any runs.

### 9. Names for the stock effects — decision (low effort)
Working names (craft nouns, in the family of your products): EQ **Facet**,
compressor/limiter **Ballast**, gate/expander **Sluice**, transient shaper
**Flint**, saturation **Kiln**, delay **Echelon**, reverb **Nave**,
utility **Plumb**, analyzer **Gauge** (`docs/specs/stock-effects.md`). Code
uses functional ids (`eq`, `comp`, …) until you confirm, so vetoes are free.

### 10. Listen to Orrery — listening check (15 minutes, when convenient)
Orrery is playable: `Orrery.clap` (release build) is installed in `~/.clap`
(with its LICENSE/NOTICES) and loads in REAPER with its editor. Two ways to
listen:
- **The ten starting recipes** from the agent guide
  (`skill/guides/sound-design.md`), rendered through the sandbox's MCP server:
  `renders/listening/orrery-recipes/01-warm-pad.wav` … `10-string-ensemble.wav`
  (regenerate: `tools/listening/render_orrery_recipes.py`). Levels are not
  normalized (−19 to −33 LUFS; the snare and pluck are quiet).
- **In REAPER:** insert Orrery on an instrument track and play; the editor
  adds modules (click the dashed tiles), changes types, and adds routes.
What I'd like to know: which recipes sound bad or cheap and why (harsh top,
thin, static, wrong envelope…), whether the wavetables (`bell`, `formant`,
`organ`, `digital`) are worth keeping as designed, and anything in the editor
that is confusing. Descriptor numbers say the recipes hit their targets; only
you can say whether they sound good.
*Meanwhile:* performance work continues (18 % → 11.7 % of a core for the
reference patch so far; target 3 %).

### 11. B1 at a 64-frame period — scheduling (10 minutes of your machine)
The Phase 4 acceptance benchmark needs the audio device at 64 frames. Quire
asks for it per client (`PIPEWIRE_LATENCY`), which changes the whole
PipeWire graph's quantum while the benchmark runs (your other audio apps
too); nothing is written to any config and it ends with the process. I will
run it only when you say it is a good time (and with the stock EQ and
compressor, which are being built now).
*Meanwhile:* preliminary numbers are in `docs/benchmarks/b1-preliminary.md`.

### 12. Template-built plugins export fmt's GNU-unique symbols — FYI
Starter.vst3 and Vellum.vst3 (both from the template) export fmt's
`STB_GNU_UNIQUE` symbols (e.g. `fmt::…::format_facet<std::locale>::id`).
Loaded into one process, the second binds to the first's copies; unloading
the first then leaves dangling references (Quire's VST3 scan crashed at exit
until it stopped unloading modules, which is how it works now). A fix in the
template would be hidden visibility for everything but the entry points (a
linker version script, or `-fvisibility=hidden` on the dependencies). I have
not touched the template or your plug-ins; say if you want a patch proposed.

## Resolved

(none yet)
