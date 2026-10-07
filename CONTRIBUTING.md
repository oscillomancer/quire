# Contributing

Thanks for looking at Quire. The engineering rules in `AGENTS.md` apply to
every contribution, whether you write it yourself or with a coding agent.
This page covers the process around them.

## Reporting a problem

Open an issue with:

- what you did, what you expected and what happened;
- the Quire commit you built, your distribution, and your audio setup
  (PipeWire or JACK, buffer size, sample rate);
- the smallest project file that shows it, when one does (Quire projects
  are JSON, with their media in the `media/` folder beside them);
- for crashes, the terminal output and a backtrace if you can get one.

Plugin problems are easier to pin down with the plugin's name, format
(CLAP or VST3), version and isolation mode.

## Proposing a change

For anything larger than a fix, open an issue first and describe the
change. Design changes that touch a file format, a command's schema, the
control protocol or a library's public interface need a spec update, and a
change of direction needs an ADR (`docs/decisions/README.md` explains the
format).

A pull request should:

- do one thing, with commits staged from explicit paths and subjects in the
  form `Area: what changed`;
- include tests that fail without the change (fixes) or cover it
  (features);
- update the spec, the generated files (`tools/gen/update-generated.sh`)
  and `STATUS.md` where they are affected;
- pass `tools/ci/run-all.sh` locally; say in the description which presets
  you ran if you could not run them all;
- state what you verified and how, including anything checked by hand in
  the GUI.

Code generated with an agent is welcome on the same terms: you are
responsible for every line, and you must have run what you claim works.

## Licensing of contributions

Quire does not have a license yet, so outside contributions cannot be
accepted until one is chosen. Contributions must never include code copied
from GPL or AGPL projects, or from REAPER (see the hard rules in
`AGENTS.md`).
