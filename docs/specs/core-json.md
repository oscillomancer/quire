# Spec: core-json and core-schema

**Status:** Frozen for Phase 1 implementation (2026-09-26). Changes via ADR.
**Libraries:** `libs/core-json` (`supple::json`), `libs/core-schema`
(`supple::schema`). Both depend only on core-base and the C++ standard
library. Neither may be used on the audio thread.

These two libraries carry every structured document and every command in
Quire. Their quality bar is the error messages: an LLM that sends a bad patch
or command must be able to fix it from the error alone. See ADR-0009 for the
format rules they implement.

---

## Part A — core-json

Header layout: `supple/json/value.h`, `parse.h`, `write.h`, `pointer.h`,
`merge.h` (+ `json.h` including all).

### A.1 Value

```cpp
namespace supple::json {

enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

class Value;
struct Member;                       // { std::string key; Value value; }
using Array = std::vector<Value>;

// Ordered object: preserves insertion order. Keys are unique (the parser
// rejects duplicates). Lookup is linear below 16 members and hashed above
// (implementation detail; iteration order is always insertion order).
class Object {
  public:
    std::size_t size() const;
    bool empty() const;
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const;
    // Replaces the value if the key exists (keeping its position), else appends.
    Value& set(std::string key, Value v);
    bool erase(std::string_view key);            // true if it existed
    Value& operator[](std::string_view key);     // inserts null if missing
    // Iteration over Members in insertion order.
    auto begin() const; auto end() const; auto begin(); auto end();
    void sortKeys();                             // this level only; WriteOptions::sortKeys is recursive
};

class Value {
  public:
    Value();                                      // null
    Value(std::nullptr_t);
    Value(bool);
    Value(int); Value(long); Value(long long);    // -> Int
    Value(unsigned); Value(unsigned long); Value(unsigned long long); // -> Int; values > INT64_MAX -> Double
    Value(double);                                // -> Double (NaN/Inf allowed in memory, rejected by write())
    Value(const char*); Value(std::string); Value(std::string_view);
    Value(Array); Value(Object);
    static Value array(); static Value object();

    Type type() const;
    bool isNull() const; bool isBool() const; bool isInt() const; bool isDouble() const;
    bool isNumber() const;                        // Int or Double
    bool isString() const; bool isArray() const; bool isObject() const;

    // Checked accessors: assert on wrong type in debug; UB-free defaults in release
    // (false / 0 / empty) -- callers validate with a schema first.
    bool asBool() const;
    std::int64_t asInt() const;                   // Double -> truncation only if integral, else 0
    double asDouble() const;                      // Int converts exactly (|v| <= 2^53) or nearest
    const std::string& asString() const;
    const Array& asArray() const;  Array& asArray();
    const Object& asObject() const; Object& asObject();

    // Object/array conveniences. operator[] on a null Value converts it to an
    // object (key) or array (index push via push_back only).
    const Value* find(std::string_view key) const;
    Value& operator[](std::string_view key);
    const Value& at(std::size_t i) const;
    void push_back(Value v);                      // null -> array
    std::size_t size() const;                     // array/object size, else 0

    // Deep equality. Numbers compare by numeric value across Int/Double
    // (1 == 1.0). Object equality ignores member order.
    friend bool operator==(const Value&, const Value&);
};

std::string_view typeName(Type);                  // "null", "boolean", "integer", "number", "string", "array", "object"
}
```

Copying a Value deep-copies. Moves are cheap. (Structural sharing for the
session model is done above this layer.)

### A.2 Parsing

```cpp
struct ParseOptions {
    std::size_t maxDepth = 256;
    std::size_t maxBytes = 256u << 20;   // 256 MiB
};
Result<Value> parse(std::string_view text, const ParseOptions& = {});
```

- Strict RFC 8259. UTF-8 is validated (overlongs, surrogates, invalid bytes
  are errors). A UTF-8 BOM is rejected with a hint to remove it.
- Integer literals (no `.`/`e`/`E`) that fit in int64 become `Int`; all other
  numbers become `Double` (literals that overflow double are errors).
- Duplicate object keys are an error (`json.parse.duplicate_key`).
- Leading/trailing whitespace allowed; anything else after the value is an
  error.
- **Errors** are `supple::Error` with `code` from the list below, `loc`
  set (1-based line, 1-based byte column), `message` naming the problem in
  context, and `hint` telling how to fix it. The message includes a one-line
  excerpt of the offending line (truncated to ~80 chars around the column)
  and a caret line, e.g.:

  ```
  json.parse.expected_comma_or_end (3:17): expected ',' or '}' after member "cutoff"
      "cutoff": 1200 "res": 0.3
                     ^
  Hint: separate object members with commas.
  ```

  Codes: `json.parse.unexpected_end`, `unexpected_char`,
  `expected_comma_or_end`, `expected_colon`, `expected_key`,
  `trailing_comma` (detect `[1,]` / `{"a":1,}` specifically — LLMs do this),
  `invalid_number`, `number_out_of_range`, `invalid_escape`,
  `invalid_unicode`, `invalid_utf8`, `control_char_in_string`,
  `duplicate_key`, `too_deep`, `too_large`, `trailing_content`, `bom`,
  `single_quotes` (detect `'...'` specifically), `comment` (detect `//` and
  `/*` specifically, hint: use a "notes" member), `nan_or_infinity`
  (detect `NaN`, `Infinity`).

### A.3 Writing

```cpp
struct WriteOptions {
    bool pretty = true;          // false: single line, no spaces (NDJSON/wire)
    bool sortKeys = false;       // sort every object's keys by UTF-8 bytes (recursive)
    int indent = 2;
    int maxLineWidth = 100;      // for the scalar-array one-line rule
};
Result<std::string> write(const Value&, const WriteOptions& = {});  // error only for NaN/Inf
std::string writeCompact(const Value&);    // asserts finite; pretty=false
std::string writeCanonical(const Value&);  // pretty, insertion order, ADR-0009 rules
std::string formatDouble(double);          // the canonical double text
```

Pretty/canonical rules (ADR-0009): 2-space indent, `": "`, `\n`, trailing
newline for top-level `writeCanonical`, members one per line, empty
containers `{}`/`[]`, arrays of only scalars on one line when the whole line
(including indentation and any key prefix) fits in `maxLineWidth`, otherwise
one element per line; Int printed as integer; Double printed as shortest
round-trip text (`std::to_chars`) containing `.` or `e` (append `.0` if
needed), `-0.0` prints `0.0`, exponent form as `to_chars` produces it (e.g.
`1e-07` is fine; do not reformat); strings raw UTF-8 with escapes only for
`"`, `\\`, `\n`, `\t`, `\r`, `\b`, `\f`, other controls `\u00XX` (lowercase hex).

**Invariant (tested, fuzzed):** for any `v` that writes successfully,
`parse(writeCanonical(v))` equals `v` exactly (same types, same member
order), and `writeCanonical(parse(writeCanonical(v)))` is byte-identical.

### A.4 JSON Pointer (RFC 6901)

```cpp
std::string pointerEscape(std::string_view token);                 // ~ -> ~0, / -> ~1
std::string pointerJoin(std::string_view base, std::string_view key);
std::string pointerJoin(std::string_view base, std::size_t index);
Result<std::vector<std::string>> pointerSplit(std::string_view pointer);
const Value* resolve(const Value& root, std::string_view pointer);  // nullptr if absent
```

### A.5 Merge patch (RFC 7396) and diff

```cpp
Value mergePatch(const Value& target, const Value& patch);   // RFC 7396 exactly
Value createMergePatch(const Value& from, const Value& to);  // minimal patch p with mergePatch(from, p) == to;
                                                             // arrays replaced whole; for non-object `to`, p = to
struct Change {
    enum class Op { Add, Remove, Replace } op;
    std::string path;           // JSON Pointer
    Value before, after;        // null when not applicable
};
std::vector<Change> diff(const Value& from, const Value& to);
// Objects recurse by key (from's order, then to's new keys). Arrays of equal
// length recurse by index; arrays of different length are one Replace.
// Numbers equal by value are equal.
```

Note: RFC 7396 cannot express "set to null" (null deletes). Documents
therefore never use `null` as a meaningful value (schemas treat absence as
the null state).

---

## Part B — core-schema

Header layout: `supple/schema/schema.h` (builders), `export.h`
(JSON Schema), `errors.h` (error codes, closest-match helper).

core-schema binds JSON to C++ types and back, validates with complete,
actionable error lists, and exports JSON Schema (draft 2020-12). One schema
declaration drives all three, so docs, validation and types never disagree.

### B.1 Declaring schemas

Schemas are values built with fluent builders. Typed schemas are
`schema::Schema<T>`; the builder functions below return them. Everything is
declared with member pointers — no macros, no codegen.

```cpp
namespace supple::schema {

// Scalars
Schema<bool>          boolean();
Schema<std::int64_t>  integer();                 // also int / uint32_t via integerOf<T>()
template <class I> Schema<I> integerOf();
Schema<double>        number();
Schema<std::string>   string();
Schema<json::Value>   any();                     // free-form JSON (validated by optional callback)

// Enums: C++ enum <-> string choices. Order is the documented order.
template <class E> Schema<E> enumeration(std::initializer_list<std::pair<E, std::string_view>> choices);

// Containers
template <class T> Schema<std::vector<T>> arrayOf(Schema<T> item);
template <class T> Schema<std::map<std::string, T>> mapOf(Schema<T> value);      // ID-keyed collections
template <class T> Schema<std::optional<T>> optional(Schema<T> inner);           // usually implicit, see fields

// Objects
template <class T> ObjectBuilder<T> object(std::string_view title);
// Tagged unions (discriminated by a string member, default "type")
template <class V> VariantBuilder<V> variant(std::string_view title, std::string_view tag = "type"); // V = std::variant<...>

// Named references for recursion / reuse in exported JSON Schema ($defs)
template <class T> Schema<T> named(std::string_view defName, Schema<T> s);
}
```

Constraint and doc modifiers on any `Schema<T>` (each returns a modified copy):

```cpp
.doc("One sentence, shown in errors, docs and MCP tool schemas.")
.example(value)                      // T; exported as "examples"
.deprecated("use X instead")
// numbers
.min(x) .max(x) .range(lo, hi) .exclusiveMin(x) .exclusiveMax(x)
.unit(Unit::Hz)                      // see B.3; exported as "x-unit"; enables unit strings
// strings
.minLength(n) .maxLength(n) .pattern("^[a-z][a-z0-9_]*$", "lowercase identifier")  // ECMA regex; the label is used in messages
.format("id:tr")                     // free-form tag exported as "format"
// arrays / maps
.minItems(n) .maxItems(n) .uniqueItems()
.keyPattern("^[a-z][a-z0-9_]*$", "module id")   // mapOf only
// custom semantic check after structural validation
.check([](const T& v, CheckCtx& c) { if (...) c.error("code.suffix", "message", "hint"); })
```

Objects:

```cpp
struct FilterSpec { Mode mode; double cutoff; double resonance; std::optional<double> drive; };

auto filterSchema = schema::object<FilterSpec>("filter")
    .doc("A resonant filter.")
    .field("mode", &FilterSpec::mode, modeSchema)                              // required
    .field("cutoff", &FilterSpec::cutoff, number().range(20, 20000).unit(Unit::Hz))
        .defaultValue(1000.0)                                                  // makes it optional in JSON
    .field("resonance", &FilterSpec::resonance, number().range(0, 1))
        .defaultValue(0.0)
    .field("drive", &FilterSpec::drive, number().range(0, 24).unit(Unit::dB))  // std::optional member => optional
    .check([](const FilterSpec& f, CheckCtx& c) { ... })                        // cross-field rules
    .build();                                                                  // -> Schema<FilterSpec>
```

Rules:
- A field is **required** unless its member is `std::optional<...>` or it has
  a `.defaultValue(...)`. Missing fields with defaults decode to the default.
- `.field(...)` order is the canonical member order for `encode()`.
- `encode()` omits `std::optional` members that are empty; it always writes
  fields that have defaults (explicit beats implicit in saved documents).
  An `.omitIfDefault()` field modifier exists for noisy fields.
- `ObjectBuilder<T>::unknownMembers(Policy)` sets the per-object policy
  (default: inherit from DecodeOptions).
- Nested objects, arrays of objects, maps of variants all compose.
- `ObjectBuilder<T>::field` works with member pointers of any supported type
  (`bool`, integral types, `double`, `float`, `std::string`, enums with
  enumeration schemas, `std::vector`, `std::map<std::string,_>`,
  `std::optional`, `std::variant` with variant schemas, `json::Value`, and any
  struct with an object schema).

Variants:

```cpp
using Module = std::variant<VaOsc, Noise, Filter>;
auto moduleSchema = schema::variant<Module>("module", "type")
    .alternative<VaOsc>("va_osc", vaOscSchema)
    .alternative<Noise>("noise", noiseSchema)
    .alternative<Filter>("filter", filterSchema)
    .build();
// JSON: {"type": "va_osc", "wave": "saw", ...}; the tag member is written first.
// Unknown tag -> schema.unknown_variant with the list of valid tags and the closest match.
```

### B.2 Decoding, encoding, validating

```cpp
enum class UnknownMembers { Error, Ignore };
struct DecodeOptions {
    UnknownMembers unknownMembers = UnknownMembers::Error;
    bool acceptUnitStrings = true;      // "1.2 kHz" for number fields that declare a unit
    bool coerceIntegralDoubles = true;  // 3.0 accepted for integer fields
    std::size_t maxErrors = 50;         // after this, stop and add "schema.too_many_errors"
};
struct Warning { std::string code, message, path; };

template <class T> class Schema {
  public:
    Result<T> decode(const json::Value&, const DecodeOptions& = {}, std::vector<Warning>* warnings = nullptr) const;
    json::Value encode(const T&) const;             // canonical member order
    Status validate(const json::Value&, const DecodeOptions& = {}) const;  // decode without keeping the value
    json::Value jsonSchema() const;                 // draft 2020-12, see B.4
    // ... builder modifiers from B.1
};
```

- `decode` validates **everything** it can and returns one `Error` with
  code `schema.invalid`, message `"N problems in <title>"`, and `details`
  holding one `Error` per problem, each with a JSON Pointer `path`. With a
  single problem the top-level error is that problem itself (no wrapper).
- `Int` JSON values decode into `double` fields (always). `Double` JSON values
  decode into integer fields only if integral and `coerceIntegralDoubles`.
- Numbers must be finite. Integer fields check the target type's range.
- For fields with a unit and `acceptUnitStrings`, a JSON string such as
  `"1.2 kHz"`, `"-3dB"`, `"250 ms"`, `"0.25 s"` is parsed (B.3), converted to
  the field's canonical unit, then range-checked. Incompatible units are an
  error naming the expected dimension.
- With `UnknownMembers::Ignore`, unknown members produce `Warning`s instead of
  errors (used for newer-minor documents per ADR-0009).

Error codes (all `schema.*`), each with the stated hint content:

| code | when | hint must contain |
|---|---|---|
| `schema.type` | wrong JSON type | expected type and a valid example from `.example()` or the default |
| `schema.required` | missing required member | the member's doc line and type |
| `schema.unknown_member` | member not in schema | closest known member name ("did you mean \"cutoff\"?") and the list of valid members if ≤ 12 |
| `schema.out_of_range` | below min / above max | the valid range with unit, e.g. "valid range: 20 .. 20000 Hz" |
| `schema.not_finite` | NaN/Inf reached decode (only possible programmatically) | — |
| `schema.integer_required` | non-integral number for integer field | nearest integer |
| `schema.enum` | string not among choices | closest choice + all choices |
| `schema.unknown_variant` | tag value not among alternatives | closest tag + all tags |
| `schema.missing_tag` | variant object without tag | tag member name + all tags |
| `schema.length` | string too short/long | limits |
| `schema.pattern` | regex mismatch | the pattern's label |
| `schema.items` | array too short/long | limits |
| `schema.unique` | duplicate items | index of first duplicate |
| `schema.key` | map key fails keyPattern | the pattern's label |
| `schema.unit` | unit string unparsable or wrong dimension | expected unit and an example string |
| `schema.check.<name>` | custom `.check` failure | whatever the check supplies |
| `schema.too_many_errors` | error cap reached | — |

Closest-match: Damerau–Levenshtein distance ≤ max(2, len/3), case-insensitive,
ties broken by declaration order; also match when one is a prefix of the
other. Exposed as `supple::schema::closestMatch(std::string_view, std::span<const std::string_view>)`
for reuse (command names, plugin keys, notation).

### B.3 Units

*(Amended 2026-09-26: units live in core-base as `supple/base/units.h`,
namespace `supple::units`, so plugins can use them without core-schema;
`closestMatch` likewise lives in core-base as `supple/base/closest.h`.)*

`supple/base/units.h` defines the closed unit set used across Quire. Each
unit belongs to a dimension; each dimension has one canonical unit; strings
may use any alias of a compatible unit.

| Unit (canonical form) | Dimension | Accepted aliases in strings | Conversion to canonical |
|---|---|---|---|
| `Hz` | frequency | `hz`, `Hz`, `kHz`, `khz`, `k` (only after a number: `1.2k`) | kHz ×1000 |
| `ms` | time | `ms`, `s`, `sec`, `us`, `µs` | s ×1000, us ÷1000 |
| `dB` | level/gain | `dB`, `db`, `dBFS` (only for fields tagged as absolute level) | — |
| `LUFS` | loudness | `LUFS`, `LKFS` | — |
| `LU` | loudness difference | `LU` | — |
| `st` | pitch interval | `st`, `semi`, `semitones` | — |
| `ct` | fine pitch | `ct`, `cent`, `cents` | — |
| `oct` | pitch interval (octaves) | `oct`, `octave(s)` | — (separate dimension from st on purpose: env amounts in octaves) |
| `%` | proportion | `%` | stored as percent value 0..100 |
| `ratio` | compression ratio | `:1` suffix (`4:1`), `ratio` | 4:1 → 4 |
| `x` | multiplier | `x`, `×` | — |
| `deg` | angle | `deg`, `°` | — |
| `bpm` | tempo | `bpm` | — |
| `beats` | musical time | `beats`, `qn` | — |
| `samples` | sample count | `samples`, `smp` | — |
| `none` | dimensionless | — | — |

Time uses **ms** as the canonical unit everywhere (`s` is only a string
alias). `%` fields store 0..100. A field's unit
must be the canonical unit of its dimension; builders assert this.

API: `Result<double> parseQuantity(std::string_view text, Unit fieldUnit)`
(accepts leading `+`/`-`, decimals, exponent, optional space, alias;
bare numbers in strings are accepted as the canonical unit);
`std::string formatQuantity(double v, Unit u, int significantDigits = 3)` for
display ("1.20 kHz", "250 ms", "-3.0 dB", "4.0:1", "+7 st"); `unitName(Unit)`
and `dimensionName(Unit)`.

### B.4 JSON Schema export

`jsonSchema()` returns a draft 2020-12 schema suitable for MCP tool input
schemas: `type`, `properties` (in field order), `required`,
`additionalProperties: false` (for Error policy), `description` from `.doc`,
`default`, `minimum`/`maximum`/`exclusive*`, `minLength`/`maxLength`,
`pattern`, `enum` for enumerations, `oneOf` + `const` discriminators for
variants (with `"discriminator"` hint member ignored by validators),
`items`, `minItems`/`maxItems`/`uniqueItems`, `additionalProperties` +
`propertyNames.pattern` for maps, `examples`, `deprecated`, and extension
keywords `x-unit` (canonical unit string) and `x-unit-strings: true` when
unit strings are accepted — in which case `type` is `["number", "string"]`.
`named()` schemas go to `$defs` and are referenced with `$ref`.

### B.5 Performance and safety

Decoding a 1 MB document must take well under 50 ms in Release. No global
state; schemas are immutable after `build()` and safe to share across
threads. Recursion depth bounded by core-json's parse depth.

---

## Tests required (both libraries)

- core-json: every error code triggered with a checked message/location;
  RFC 8259 conformance cases (a corpus of accept/reject files in
  `libs/core-json/tests/corpus/`); round-trip and idempotence invariants;
  canonical formatting golden files; RFC 6901 examples; RFC 7396 appendix A
  examples verbatim; `createMergePatch` property test (random documents:
  `mergePatch(a, createMergePatch(a, b)) == b`); diff cases.
- A libFuzzer target `core-json.fuzz` (clang `-fsanitize=fuzzer,address`)
  exercising parse → write → parse under `tests/fuzz/`, built only when
  `SUPPLE_BUILD_FUZZERS=ON`, plus a short deterministic smoke run in CTest
  using a checked-in seed corpus.
- core-schema: a realistic nested fixture (object with nested objects, enums,
  optional, defaults, arrays, maps, variants) testing decode/encode round
  trips, every error code with its hint text, multiple simultaneous errors
  with correct JSON Pointer paths, unit strings (every alias row above),
  unknown-member policies with warnings, and exported JSON Schema checked
  against golden JSON files.
