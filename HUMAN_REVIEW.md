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

### 9. Names for the stock effects — decision (low effort)
Working names (craft nouns, in the family of your products): EQ **Facet**,
compressor/limiter **Ballast**, gate/expander **Sluice**, transient shaper
**Flint**, saturation **Kiln**, delay **Echelon**, reverb **Nave**,
utility **Plumb**, analyzer **Gauge** (`docs/specs/stock-effects.md`). Code
uses functional ids (`eq`, `comp`, …) until you confirm, so vetoes are free.

## Resolved

### 8. Which models and budget for automated LLM evaluations — decision
**Answer (2026-09-30):** use Hermes agents (`hermes-team`) with their default model — cheap and capable enough; as many runs as useful. *Follow-up:* the harness gets a Hermes runner and the first runs (STATUS P3.4).

### 10. Listen to Orrery — listening check (15 minutes, when convenient)
**Answer (2026-09-30):** very impressed — the recipes sound production-ready and fit their descriptions; using Orrery in REAPER feels good and makes nice sounds easy for a non-synthesist. Two notes: the FM electric piano is harsher in the highs than an electric piano should be (likely the recipe, not the synth), and the `digital` and `harmonic ramp` wavetables sound the same. *Follow-up:* darker EP recipe; make the two tables distinct or merge them.

### 11. B1 at a 64-frame period — scheduling (10 minutes of your machine)
**Answer (2026-09-30):** approved — run B1 and change the quantum as needed, now or when the workers are done. *Plan:* after the Console 9 strips land (so B1 measures the final per-strip cost) and with no builds competing for the CPU.

### 13. Console 9 gain staging and the off switch — decision (low effort)
**Answer (2026-09-30):** keeping Chris's staging is right. "Console 9 off" keeps Console 9's gains and pan law and drops only the curves and dither (level-matched on/off). ADR-0019 §6/§11 amended.

### 12. Template-built plugins export fmt's GNU-unique symbols — FYI
**Resolved (2026-09-30):** Nate fixed the template (commit 3d32b8d: explicit Linux export lists + the clap-wrapper self-handle patch); the orchestrator reviewed it and is rolling the same fix into every Quire plugin copy and the vendored clap-wrapper, with a CLAP unload test per plugin.
