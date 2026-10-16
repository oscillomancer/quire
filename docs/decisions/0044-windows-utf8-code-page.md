# ADR-0044: Windows processes use the UTF-8 code page

**Status:** Accepted, 2026-10-16. Amends ADR-0034 and ADR-0035 (minimum
Windows version).

## Context

On Windows, narrow text (`std::filesystem::path::string()`, `getenv`,
`_putenv_s`, narrow file APIs) uses the process code page, which follows
the system locale (Windows-1252 on a Western system, Shift-JIS on a
Japanese one) rather than UTF-8. The code converts paths to narrow text in
about 145 places: media paths in project files, render paths in command
results, the script library folder handed to the script player in
`SUPPLE_SCRIPT_LIBRARY`, error messages. A path with a character outside
ASCII then reached UTF-8 consumers (canonical JSON, the player's UTF-8 path
decoding, macOS and Linux reading the same project) in the legacy encoding,
and a character outside the system's code page made the conversion throw.
macOS and Linux are unaffected: their narrow paths are UTF-8.

## Decision

Every executable target on Windows embeds an application manifest that sets
`activeCodePage` to UTF-8 (`cmake/windows/utf8.manifest`, attached by
`supple_windows_utf8_manifests` at the end of the root CMakeLists). Narrow
text in Quire's processes (the app, engine, MCP proxy, sandbox, bridge,
scanner and every test) is then UTF-8 whatever the system locale, so the
existing conversions are correct and new code needs no special care. The
minimum Windows version rises from Windows 10 1803 to 1903, the first
release that honours the setting; `NTDDI_VERSION` follows.

Rejected: converting every path explicitly through UTF-8 helpers. It works
on 1803, but it touches every conversion and any new `string()` call
reintroduces the bug.

## Consequences

Plug-in DLLs do not carry the setting; it belongs to the executable. The
script player reads `SUPPLE_SCRIPT_LIBRARY` only inside Quire's own
processes, which have it. Exports loaded by other hosts locate their files
through native wide paths (ADR-0032) and are unaffected. Console windows
keep their own output code page, so non-ASCII text printed to a console can
look wrong there; piped output (MCP, scripts) is UTF-8. A Windows test
checks the process code page and a non-ASCII user folder through
`std::filesystem`, the CRT environment and the inherited environment.
