# ADR-0043: No built-in agent panel

**Status:** Accepted, 2026-10-16. Amends ADR-0039 (its agent HTTP client and
agent config parts) and gui.md §5.

## Context

The app carried an agent panel (`daw/agent-panel`): a chat with an Anthropic
or OpenAI-compatible endpoint that edited the session on its own branch
through an in-process MCP server, with a diff, A/B and accept/reject. Its
tools were the MCP server's tools; it added no capability an MCP client lacks.
A robust harness needs context management and compaction, retries, prompt
caching, reasoning continuity and more. Building and maintaining that in the
DAW duplicates what external harnesses (Claude Code, Codex, Hermes, …)
already do better, and the intended way for language models to use Quire is
through its MCP server from such a harness.

## Decision

Remove the agent panel, its HTTP client and its API-key configuration.
Agents use the MCP server (`<slug>-mcp` for the running app, `<slug>-engine
--mcp` headless). libcurl is no longer a dependency on any platform. The
command bus's scoped dispatch admission stays (the GUI uses it).

What the panel had beyond the tools, and where it goes:

- Branch review: the history dock already switches, merges and deletes
  branches; switching is the A/B. An agent can describe its changes from
  `branch.diff`.
- Listening requests: the skill already tells agents to ask the human to
  listen.
- Branch isolation and pinning (the agent could not switch branches, and its
  turn stopped if the branch changed under it): not carried over. External
  agents' edits land on the current branch. Pinning an MCP session to a
  branch can be added to the server if that proves necessary.

## Consequences

About 2,800 lines of code and tests, the libcurl dependency and the
owner-only secret storage are gone. Users who want an agent install a harness
and register the MCP server (`docs/guides/manual.md`). UI files with
`panels.agent` still load; the key is ignored.
