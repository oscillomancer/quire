# ADR-0010: Semantic parameters and value domains

**Status:** Accepted, 2026-09-26. Detailed contract: `docs/specs/core-params.md`
and `docs/specs/semantic-params-extension.md`.

## Context

Principle 6: stock plugins expose parameters with physical units, real
ranges, meaningful names and tags; values sent over the control API are in
those units, not normalized. The same plugins must still behave well in
generic CLAP hosts (REAPER is part of each plugin's acceptance), whose
automation lanes map a parameter's `[min, max]` linearly.

## Decision

### The semantic definition

Each stock-plugin parameter is defined once (core-params) with: a stable
dotted `key` (`filter.cutoff`, `env1.attack`), a permanent CLAP `id`
(session ABI; append-only), display `name`, `group`, `type` (`float`,
`int`, `bool`, `enum`), `unit` (from a closed set with one canonical unit
per quantity — `Hz`, `ms`, `dB`, `st`, `ct`, `ratio`, `%`, … — so values
are never ambiguous; display may scale, e.g. 1200 Hz shows as "1.20 kHz"),
`min`, `max`, `default`, `curve`
(`linear`, `log`, `pow(k)`, `db`), display precision, enum `choices` with
stable string values, `tags` (roles such as `role:brightness`,
`role:level`, `role:time`, `role:width`), a one-line `description`, and
flags (`automatable`, `modulatable`, `readonly`, `hidden`).

### Three value domains, one conversion point

1. **Physical** — the bus, patches, Lua, MCP, and Quire's automation
   envelopes. Floats in the canonical unit (`1200` means 1200 Hz), enums by
   choice string (`"lowpass"`), bools as `true/false`. Text with units is
   also accepted and converted (`"1.2 kHz"`, `"-3 dB"`, `"250 ms"`,
   `"+7 st"`).
2. **Normalized** — curve position in `[0, 1]`: what generic hosts see.
   For stock plugins, the CLAP parameter value *is* the normalized position
   for continuous floats (so a REAPER envelope sweeps a log cutoff evenly),
   the integer value for `int`, the choice index for `enum`, and 0/1 for
   `bool` (stepped flags set accordingly). `value_to_text` always shows
   physical values with units.
3. **DSP** — whatever the processor wants (coefficients, samples), derived
   from physical values inside the plugin.

core-params owns every conversion (physical <-> normalized <-> text) so
host, plugin, GUI and docs agree by construction.

### Automation and modulation

Quire stores envelope points in physical units and interpolates in the
normalized (curve) domain, so "200 Hz to 2 kHz" sweeps evenly in pitch.
CLAP parameter modulation amounts are normalized-domain offsets.

### Third-party plugins

Plugins without the semantic extension expose only CLAP plain values. The
bus addresses their parameters by CLAP id or name, reports the plugin's own
value text, and accepts `{"text": "1.2 kHz"}` values converted through the
plugin's `text_to_value`. No units are invented for them.

### Dynamic topologies

CLAP parameter lists are fixed while a plugin is active. Plugins with
patch-defined structure (the flagship synth) expose a fixed pool of typed
slots; the semantic schema marks parameters of unused slots inactive, and
the patch — not the parameter list — is the primary interface. Macros are the
stable automation surface for structural patches.

## Consequences

Generic hosts get well-behaved automation; Quire and agents get physical
units. Every conversion is tested once, in core-params. A parameter's curve is
part of its ABI (changing it changes what old automation means), so it is
append/rename-with-migration only.

## Alternatives considered

Physical values as the CLAP domain (linear envelopes over log quantities in
every generic host); normalized values on the bus (the anonymous-knob problem
this project exists to fix).

## Engine notification contract (review round 3)

A parameter rescan re-resolves automation against the current list and schema
on main, then publishes a new immutable plan. An orphaned lane stops sending
and warns with FX id and parameter; its envelope remains available for repair.
Clear ALL/AUTOMATIONS suppresses only that parameter until rescan or reload;
clear MODULATIONS does not disable an automation lane. No clear deletes user data.

The engine's bounded output-event queues carry complete flags and voice addresses
into a main-thread take API. A future app consumer stores plain/physical values,
combines gesture boundaries into one undo step, honors DontRecord, and never
sends a received value back to DSP. It must acknowledge semantic patch updates
in the instance pool before rebuilding a plan, and recover from counted overflow.
See engine.md's plug-in notification contract for capacities and integration.
