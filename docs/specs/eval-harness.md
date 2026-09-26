# Spec: the LLM usability evaluation harness (P3.4, extended in Phase 6)

**Status:** Draft for review (2026-09-26).
**Code:** `tools/eval-harness/` (Python 3 tooling — allowed by ADR-0003
because it never ships in a runtime path).

Quire's claim is that agents can program its plugins and operate its DAW.
The harness turns that claim into numbers tracked across plugin, skill and
model revisions — and queues what numbers cannot judge for Nate's ears.

## 1. Pieces

- **Tasks** — `tools/eval-harness/tasks/<suite>/<task>.json`:
  ```json
  {
    "schema": "supple.eval-task/1.0",
    "id": "synth.pluck-bass",
    "suite": "synth-descriptive",
    "prompt": "Program Orrery as a plucky bass with a short filter snap. Play it with the phrase given.",
    "phrase": "notation 1\nvoice bass ch 1\n1: C2:e r:e C2:e Eb2:e r:e G1:e Bb1:q",
    "setup": [{"command": "plugin.load", "args": {"plugin": "com.suppleaudio.orrery"}}],
    "score": {"profile": { …supple.analysis-profile… }, "reference": null},
    "limits": {"tool_calls": 60, "minutes": 10},
    "listening": "Is the snap audible and short? Does it sound like a bass, not a lead?"
  }
  ```
  Reference-matching tasks carry a hidden patch instead: the harness renders
  it before the run and gives the agent only the render id (never the
  patch).
- **Runners** — adapters that give a model the MCP server and the task:
  - `claude-cli`: `claude -p` with an MCP config pointing at
    `<slug>-sandbox --mcp --workdir <run dir>`, the skill directory, and a
    tool allow-list restricted to the sandbox tools;
  - `codex-cli`: `codex exec` with the same MCP server;
  - `api`: a minimal tool-use loop in Python for the Anthropic Messages API
    or any OpenAI-compatible endpoint (including a local llama-server), with
    MCP tools bridged as function tools.
  Runners record the full transcript (tool calls, results, final message)
  and token/usage counts when available.
- **Scorer** — after the run, the harness renders the task phrase through
  the agent's final patch (fresh session, same seed), analyzes it, and
  computes: profile pass fraction and loss; for reference tasks, the
  optimizer's reference loss (optimizer spec §3) and LTAS RMS dB; validity
  (did it end with a valid patch; error count; did it use `patch.merge` /
  batches sensibly); efficiency (tool calls, wall time, tokens).
- **Reports** — `docs/benchmarks/eval/<yyyy-mm-dd>-<suite>-<runner>-<model>.json`
  (every task, every metric, transcript path) plus a Markdown summary table;
  `docs/benchmarks/synth-eval.md` keeps the history (one row per run: date,
  git hash, skill version, runner/model, pass rate, median loss).
- **Listening queue** — each run writes the best and worst render per task
  to `docs/benchmarks/eval/audio/<run>/` (gitignored) and appends one
  `HUMAN_REVIEW.md` item per suite run: file paths plus the task's
  `listening` question.

## 2. Suites

- `synth-descriptive` (10 tasks, synth spec §10): pad, pluck bass, bell,
  supersaw lead, sub bass, FM e-piano, noise snare, formant lead, acid bass,
  strings. Profiles are ranges wide enough to be fair and narrow enough to
  mean something; they are validated by hand-made reference patches that
  must pass their own profiles.
- `synth-reference` (10 tasks): hidden patches spanning the module set;
  score by reference loss; tiered difficulty (1–3 modules changed from a
  template → full patch from scratch).
- `notation` (Phase 1–2 onward): write a described phrase; scored by compile
  success on the first attempt and by structural checks (bar count, meter,
  pitch range, rhythm density).
- `mixing` and `session` (Phase 6): full-session tasks (arrange a 16-bar
  loop in a described style; fix kick/bass masking; deliver stems at
  −14 LUFS) scored by analysis of the renders and by session checks.

## 3. Rules

- The harness never modifies the repository except `docs/benchmarks/`.
- Runs are reproducible: task files, skill version (git hash), seeds and
  runner/model identifiers are recorded; the sandbox is deterministic.
- Model endpoints and spending are Nate's call (`HUMAN_REVIEW.md`); the
  harness defaults to a dry-run that validates tasks and scorers without
  calling any model.
