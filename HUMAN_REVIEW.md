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

## Resolved

(none yet)
