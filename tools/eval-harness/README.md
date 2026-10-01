# LLM usability evaluation

Python 3, standard library only. Build the dev sandbox and Orrery first:

```sh
cmake --preset dev
cmake --build --preset dev -j6
python3 tools/eval-harness/run.py --dry-run
python3 tools/eval-harness/run.py --suite synth-reference --dry-run
python3 tools/eval-harness/run.py --suite notation --dry-run
python3 -m unittest discover -s tools/eval-harness -v
SUPPLE_EVAL_INTEGRATION=1 python3 -m unittest discover -s tools/eval-harness -v
```

The default runner is `dry`, with one repeat and seed 42. It submits a known
recipe/hidden patch or phrase through the real sandbox and runs the same scorer
as Hermes. A dry validation exits nonzero if any scripted answer fails. It
writes its report into ignored `build/eval/`; add `--publish` to retain a
benchmark and listening item. CTest runs the cheap unit tests; the optional
integration checks need a dev build and never invoke a model.

A model call must be explicit:

```sh
python3 tools/eval-harness/run.py --suite synth-descriptive --runner hermes \
  --tasks sub-bass --repeat 1
```

`--jobs N` (1..16) runs attempts concurrently; each already has its own run
directory, Hermes home and sandbox, and the report keeps task/repeat order.
Reference attempts take minutes each, so use it for full suites. SIGINT/SIGTERM
stop every live attempt's process group.

`--tasks` accepts full ids or suffixes, separated by spaces or commas. Suites
are `synth-descriptive` (10), `synth-reference` (10), and `notation` (5).
`--dry-run` always forces the scripted runner, including with `--runner hermes`.
The orchestrator chooses real run counts. No model-quality conclusion should
be drawn from the smoke run or from the dry runner.

Hermes gets an isolated `HERMES_HOME` for every attempt. The harness reads only
the pinned model/provider identity from
`~/.hermes/profiles/quire-eval/config.yaml` and references
`~/.hermes/profiles/quire-eval/.env` with a symlink; it never copies credentials. Override these
sources with `--model-config` and `--credentials`. The model mapping may be
simple YAML or JSON; no other source-profile settings or MCP servers are
inherited. The profile name `quire-eval` is recorded; missing profile files fail
without falling back to `~/.hermes/config.yaml`. The expected pinned identity is
`meta/muse-spark-1.3-contributor` via `openrouter`. OAuth credential stores are not copied or refreshed by the harness;
use environment/API-key credentials for this runner. Process-environment
`HERMES_*` overrides are removed before the isolated settings are applied.

The only toolset is `sandbox`. The repository skill and relevant guides are
in the prompt, since file/skill/shell/web tools are unavailable. `--ignore-rules`
prevents workspace/private AGENTS, memory and persona injection. The installed
Hermes CLI performs inference and session export. For a managed Hermes install
whose standard launcher wants to write installation locks, use its **existing**
venv interpreter with `--hermes-python /path/to/existing/venv/bin/python`; this
runs `-I -B -m hermes_cli.main` and disables lazy installs. No package manager,
installation, or default-profile edit is performed. Before any model call the
runner checks that the CLI starts under an isolated home (`config path`) and
stops with this hint if it does not (on this workstation:
`--hermes-python ~/.hermes/hermes-agent/venv/bin/python`). A run in which no
attempt reached the model publishes nothing. `--hermes-bin` selects a
normal CLI executable on other installations.

`relay.py` launches the product-slug-derived sandbox binary with `--mcp
--record-midi --workdir <attempt>`. It forwards only that server's MCP surface,
records every model tool request/result, and asks the live sandbox for its
chain and full patches after each call and on clean shutdown. Internal queries
and task setup do not count toward the model call budget. The relay stops when
the call limit is exceeded; a process-group deadline kills Hermes and all its
children. The last completed capture survives interruption. The harness exports
Hermes sessions with `--redact`, records usage (including auxiliaries), and
scores captured patches independently of the final prose.

A fresh sandbox restores the fixed settings/seed and renders the fixed phrase.
Descriptive scores use production `profile.evaluate`, with pass fraction,
weighted normalized profile distance, and a squared-distance loss. Reference
scores use the production C++ optimizer loss directly through the sandbox's
non-MCP `eval-reference` tooling entry, with explicit note-off times for both
files and production loudness matching; the MCP `compare` command reports that
same number as `loss.total`, so an attempt can read the score it is judged by.
There is no rounded-JSON reimplementation
or optimizer search in scoring. Notation scores use compiler-produced note data
in `<render>.midi.json`: first attempted render compilation, bars, all bar
meters, pitch bounds, and note attacks per bar. The last notation submitted
through render is final, even if invalid; repairing it after an initial compile
failure does not restore the first-attempt point. First-attempt compilation inside
Lua scripts is not observable as a typed render request; notation tasks therefore
require direct `render` or an explicit `batch`/`exec` containing render.

Real attempts always publish JSON/Markdown under `docs/benchmarks/eval/`, a
history row, ignored best/worst WAVs, and one active listening queue item per
suite run in `HUMAN_REVIEW.md` (dry runs publish only the benchmark files). One repeat gives identical best/worst selections.
Transcripts, usage, prompts, captures, stderr and intermediate renders remain in
`build/eval/<run>/`. These local files and the listening WAVs must be retained or
archived together if the report is to remain fully reviewable after build cleanup.
See `docs/specs/eval-harness.md` for task/profile decisions and known limits.
