# Spec: core-params — semantic parameters

**Status:** Frozen for Phase 1 implementation (2026-09-26). Changes via ADR.
**Library:** `libs/core-params` (`supple::params`). Depends on core-base
(units, closest match, Error) and core-json. Header-light: plugins include
it. The conversion functions are RT-safe (no allocation) and may be called on
the audio thread; text and JSON functions are not.
**Decision record:** ADR-0010 (value domains).

## 1. Parameter definitions

```cpp
namespace supple::params {

enum class Type : std::uint8_t { Float, Int, Bool, Enum };

enum class CurveKind : std::uint8_t {
    Linear,   // n = (v - min) / (max - min)
    Log,      // n = ln(v / min) / ln(max / min); requires 0 < min < max
    Pow,      // v = min + (max - min) * n^k; k > 0 (k > 1 gives resolution near min)
    Bipolar,  // symmetric pow around the midpoint: n in [0,1] maps to [-1,1] then v = mid + half*sign(u)*|u|^k
};
struct Curve { CurveKind kind = CurveKind::Linear; double k = 1.0; };

struct Choice {
    std::string_view key;     // stable machine value, lowercase snake: "lowpass"
    std::string_view label;   // display: "Low-pass"
};

enum Flag : std::uint32_t {
    kAutomatable = 1u << 0,
    kModulatable = 1u << 1,   // CLAP param modulation allowed
    kReadonly    = 1u << 2,   // meters/outputs; never saved in patches
    kHidden      = 1u << 3,   // not shown in generic UIs (still addressable)
    kOffAtMin    = 1u << 4,   // the minimum means "off": displays "off" / "-inf dB"
    kBypass      = 1u << 5,   // the plugin's bypass param (CLAP_PARAM_IS_BYPASS)
    kPeriodic    = 1u << 6,   // wraps (phase/angle); CLAP_PARAM_IS_PERIODIC
};

struct ParamSpec {
    std::uint32_t id;                         // CLAP param id; session ABI, append-only, never reused
    std::string_view key;                     // "filter.cutoff": [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*
    std::string_view name;                    // "Cutoff" (short, for UIs; CLAP name)
    std::string_view group;                   // "Filter" (CLAP module path; '/' separates levels)
    Type type;
    units::Unit unit;                         // Float/Int; Unit::none for Bool/Enum
    double min, max, def;                     // physical; Bool 0/1; Enum 0..choices-1 (index)
    Curve curve;                              // Float only (Int uses Linear)
    int digits;                               // significant digits for display (Float)
    std::span<const Choice> choices;          // Enum only, >= 2
    std::span<const std::string_view> tags;   // "role:brightness", "role:level", "role:time", ...
    std::string_view description;             // one sentence: what it does, in plain words
    std::uint32_t flags;
    double smoothingMs;                       // DSP hint; 0 = no smoothing
};

using Table = std::span<const ParamSpec>;
}
```

A plugin declares its table as a `constexpr` array of `ParamSpec` (this is
the template's "one parameter table" rule, upgraded). Everything else — CLAP
`param_info`, text, patches, GUI, docs — derives from it.

**Tags** use a small controlled vocabulary so agents can find "the brightness
control" on any plugin: `role:level`, `role:gain`, `role:mix`,
`role:brightness`, `role:resonance`, `role:time`, `role:rate`,
`role:depth`, `role:pitch`, `role:width`, `role:pan`, `role:drive`,
`role:threshold`, `role:ratio`, `role:attack`, `role:release`,
`role:frequency`, `role:q`, `role:mode`, `role:enable`, `role:seed`, plus
free tags `stage:<name>` (signal-flow position) and `macro`.

## 2. Validation of a table

```cpp
Status validateTable(Table);
```

Checks: ids unique; keys unique and well-formed; `min < max` (Float/Int);
`def` within range; Log with `min > 0`; Pow/Bipolar `k > 0`; Int ranges
integral; Enum has ≥ 2 choices with unique well-formed keys and integral
`def`; Bool range exactly 0..1; unit is the canonical unit of its dimension
(core-base rule); tags from the vocabulary or `stage:`/`macro`; readonly
params not modulatable; at most one `kBypass`. Every plugin runs this in a
unit test.

## 3. Value domains and conversions

```cpp
// All RT-safe, no allocation. Out-of-range inputs clamp; NaN maps to def.
double clampPhysical(const ParamSpec&, double v);        // also rounds Int/Enum/Bool
double toNormalized(const ParamSpec&, double physical);  // [0,1] along the curve
double fromNormalized(const ParamSpec&, double norm);

// The value a generic CLAP host sees (ADR-0010):
//   Float: normalized curve position [0,1]; Int: integer value; Enum: index; Bool: 0/1.
double toClap(const ParamSpec&, double physical);
double fromClap(const ParamSpec&, double clapValue);
struct ClapRange { double min, max, def; bool stepped; bool isEnum; };
ClapRange clapRange(const ParamSpec&);                   // for clap_param_info
std::uint32_t clapFlags(const ParamSpec&);               // CLAP_PARAM_IS_* from type/flags
```

Precision requirement: for every Float param and 10 000 values across the
range, `fromNormalized(toNormalized(v))` equals `v` within 1e-9 relative (1e-12
absolute near zero); endpoints exact (`toNormalized(min) == 0`, `(max) == 1`).

## 4. Text

```cpp
std::string format(const ParamSpec&, double physical);          // for humans and value_to_text
std::string formatCompact(const ParamSpec&, double physical);   // knob labels: no unit, ≤ 6 chars
Result<double> parse(const ParamSpec&, std::string_view text);  // for text_to_value, bus strings
```

- `format`: Float via `units::formatQuantity(v, unit, digits)` with display
  scaling (Hz ≥ 1000 → "1.20 kHz"; ms ≥ 1000 → "1.50 s"); `kOffAtMin` at min →
  "off" (or "-inf dB" for dB); signed display (`+3.0 dB`, `-7 st`) for
  units whose ranges include negatives; `%` shown as "50 %"; Enum → label;
  Bool → "on"/"off".
- `parse` consumes the **whole** string (trailing junk is an error — the
  template's `parseValue` weakness), trims whitespace, accepts: numbers in
  the canonical unit; unit strings of the right dimension ("1.2 kHz",
  "250ms", "-3 dB"); "off"/"-inf" for `kOffAtMin`; Enum keys or labels
  (case-insensitive) or index digits; Bool "on"/"off"/"true"/"false"/"1"/"0".
  Out of range → error `params.out_of_range` with the range in the hint (it
  does **not** silently clamp; callers that want clamping call
  `clampPhysical`). Unknown enum text → `params.enum` with the closest key.
- Round trip: `parse(format(v))` is within display precision of `v` and
  exact for Int/Enum/Bool.

## 5. JSON (bus and patches)

```cpp
json::Value toJson(const ParamSpec&, double physical);    // Float/Int: number; Enum: key string; Bool: true/false
Result<double> fromJson(const ParamSpec&, const json::Value&, bool acceptUnitStrings = true);
```

`fromJson` accepts numbers (canonical unit), unit strings, enum keys, bools,
and for Enum also the index as an integer; everything else is
`params.type` with an example in the hint. Range errors as in §4.

## 6. Lookup and description

```cpp
const ParamSpec* findById(Table, std::uint32_t id);
const ParamSpec* findByKey(Table, std::string_view key);
Error unknownKey(Table, std::string_view key, std::string_view jsonPath);  // with "did you mean" + group listing

json::Value describeTable(Table);   // the semantic schema published to hosts (below)
std::string describeParam(const ParamSpec&, double physical); // "Cutoff 1.20 kHz (filter.cutoff, 20 Hz..20 kHz, log)"
```

`describeTable` output (canonical JSON, keyed by param key, in table order):

```json
{
  "schema": "supple.param-table/1.0",
  "params": {
    "filter.cutoff": {
      "id": 12, "name": "Cutoff", "group": "Filter", "type": "float",
      "unit": "Hz", "min": 20.0, "max": 20000.0, "default": 1000.0,
      "curve": "log", "tags": ["role:brightness"],
      "description": "Low-pass corner frequency.", "flags": ["automatable", "modulatable"]
    },
    "filter.mode": {
      "id": 13, "name": "Mode", "group": "Filter", "type": "enum",
      "choices": ["lowpass", "bandpass", "highpass"], "default": "lowpass",
      "tags": ["role:mode"], "description": "...", "flags": ["automatable"]
    }
  }
}
```

`curve` is `"linear"`, `"log"`, `"pow:3"`, `"bipolar:2"`. Readonly params
appear with `"flags": ["readonly"]` and no default.

## 7. Tests required

Table validation (every rule, positive and negative); conversion precision
property tests for every curve kind; CLAP mapping per type; format/parse
round trips and every parse error with its hint; JSON I/O; `describeTable`
golden output for a fixture table; an rtsan-labelled test exercising the
RT-safe conversions inside a `SUPPLE_RT` function.

## Implementation notes (2026-09-26, orchestrator, after review round 2)

- Range checks in `parse`/`fromJson` allow a relative 1e-9 slack local to each
  boundary (`1e-9 · max(1, |boundary|)`), so a boundary value that went
  through text or JSON still counts, while a wide range no longer widens the
  slack at the other end (`[0, 1e9]` rejects `-0.5`).
- `validateTable` also requires a finite `max - min` (`params.table.range_span`)
  and log curves spanning at most 15 decades (`params.table.log_range`), so
  normalization never overflows.
- OffAtMin accepts `off`, `-inf`, `-inf dB`, `-∞` (and `-∞ dB`); a bare `inf`
  is no longer an alias for off.
