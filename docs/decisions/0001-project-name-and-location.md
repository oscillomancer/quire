# ADR-0001: Project name and repository location

**Status:** Accepted, 2026-09-26. Queued for Nate's veto in `HUMAN_REVIEW.md`.

## Context

The build prompt leaves the name to the lead engineer and asks that renaming
stay cheap. It names the location `~/projects/<name>`, with the plugin
template "alongside it" at `~/projects/audio-plugins/template`. On this
machine the directory is `~/Projects` (capital P); `~/projects` does not
exist, and the template is at `~/Projects/audio-plugins/template`.

Nate's product names are short, concrete, crafted nouns: Ferric, Loupe,
Plateau, Vellum.

## Decision

- The DAW is called **Quire**. A quire is a gathering of folded sheets
  (vellum!) sewn into a book, and the archaic spelling of *choir*. It fits the
  family, is one syllable, is easy to type as a command prefix (`quire`), and
  no audio product uses it (a web search on 2026-09-26 found none; quire.io is
  an unrelated task-management app).
- The monorepo lives at **`~/Projects/quire`**, next to
  `~/Projects/audio-plugins/`, preserving the "alongside" relationship the
  prompt intended. Creating a new `~/projects` directory would have split
  Nate's tree for no benefit.
- The name is defined once, in `cmake/Product.cmake` (`PRODUCT_NAME`,
  `PRODUCT_SLUG`). C++ reads it from the generated `<supple/product.h>`.
  Binary names, config/data directories, socket paths, MCP server names and
  UI strings derive from it. Code namespaces use the vendor (`supple::`), so a
  rename never touches source files.

## Consequences

Renaming = edit `cmake/Product.cmake`, rename the directory, grep docs. The
license gate and review both flag hard-coded names. Stock plugin names are
chosen per plugin (ADR-0011) and are not derived from the DAW name.

## Alternatives considered

Loom (taken by a well-known video product), Lathe, Stave (generic),
Palimpsest (apt for branches, too long), Folio (common), Codex (collides
with the agent tool used to build this).
