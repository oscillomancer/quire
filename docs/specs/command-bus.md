# Spec: command bus, control protocol and MCP adapter

**Status:** Frozen for Phase 2 implementation (2026-09-26). Changes via ADR.
**Decision record:** ADR-0007.
**Libraries:** `libs/command-bus` (`supple::cmd`), `libs/control-protocol`
(`supple::rpc`), `mcp/` (`supple::mcp`). All run on the model thread only
(transports marshal requests onto it). Depend on core-json and core-schema.

---

## 1. Commands

### 1.1 Naming

`<noun>[.<noun>].<verb>`: lowercase segments `[a-z][a-z0-9-]*` joined by
dots. **No underscores** (so the MCP mapping `.`→`_` is reversible). Verbs:
`add`, `remove`, `get`, `set`, `list`, `describe`, `move`, `rename`,
`merge`, `render`, `analyze`, `compare`, `load`, `save`, `scan`, `reset`.
Examples: `plugin.load`, `patch.merge`, `track.add`, `fx.param.set`,
`render.stems`. Names are permanent once released.

### 1.2 Declaration

```cpp
namespace supple::cmd {

enum class Kind : std::uint8_t { Query, Edit, Action };
enum class Exposure : std::uint8_t { McpTool, McpExec, Internal };

struct ErrorSpec { std::string code; std::string when; std::string hint; };

struct Example {
    std::string title;         // "Set a cutoff with a unit string"
    json::Value args;
    json::Value result;        // expected; strings "<any>" match any value
    std::string fixture;       // named fixture the example runs against
};

template <class Args, class Res>
struct Command {
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary;       // imperative, ≤ 80 chars: "Merge a partial patch into a plugin instance."
    std::string description;   // when/why to use; limits; related commands
    std::vector<std::string> tags;
    schema::Schema<Args> args;
    schema::Schema<Res> result;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;          // ≥ 1 required for every non-Internal command
    std::function<Result<Res>(const Args&, Context&)> run;
};

struct NoArgs {};   // with schema::object<NoArgs>("no_args").build()
}
```

`Context` is supplied by the host application (sandbox, engine):

```cpp
class Context {
  public:
    const Origin& origin() const;
    template <class S> S& service();                // typed services registered by the host; asserts presence
    void warn(std::string code, std::string message, std::string path = {});
    const std::vector<Warning>& warnings() const;
};

struct Origin {
    enum class Type : std::uint8_t { Ui, Lua, Mcp, Rpc, Test, Internal } type;
    std::string client;   // "claude-code", "gui", "script:tidy.lua"
    std::string agent;    // agent/session label if any
};
```

`Edit` commands mutate host state through services (the Phase 4 engine's
`SessionService` makes them pure `(Session, Args) → Session` functions under
the hood; ADR-0008). The bus itself is state-agnostic.

### 1.3 Registry

```cpp
class Registry {
  public:
    template <class Args, class Res> void add(Command<Args, Res>);   // asserts: name rules, unique (name, version), ≥1 example unless Internal
    const CommandInfo* find(std::string_view name, int version = 0) const;  // 0 = latest
    std::vector<const CommandInfo*> all() const;                     // sorted by name, then version
    Error unknown(std::string_view name) const;                      // command.unknown with closest names
    json::Value exportJson() const;                                  // registry document (§1.6)
};

struct CommandInfo {
    std::string name; int version; Kind kind; Exposure exposure;
    std::string summary, description; std::vector<std::string> tags;
    json::Value argsSchema, resultSchema;       // JSON Schema 2020-12
    std::vector<ErrorSpec> errors; std::vector<Example> examples;
    Result<json::Value> invoke(const json::Value& args, Context&) const;   // validate → run → encode
    Status validate(const json::Value& args) const;
};
```

The library registers its own meta commands: `registry.list` (filter by
tag/kind/text; returns name, version, kind, summary) and `registry.describe`
(one command: full schemas, errors, examples). Both `Query`, `McpExec`.

### 1.4 Dispatch

```cpp
struct Invocation {
    std::string command;          // "patch.merge" or "patch.merge@1"
    json::Value args;             // object; absent = {}
    Origin origin;
};

class Dispatcher {
  public:
    Dispatcher(const Registry&, Host&);
    Result<json::Value> execute(const Invocation&);
};

class Host {    // implemented by the application
  public:
    virtual ~Host() = default;
    virtual Context makeContext(const Origin&) = 0;
    virtual Result<std::uint64_t> beginTransaction(const Origin&, std::string_view label) = 0;
    virtual Status commitTransaction(std::uint64_t) = 0;
    virtual void rollbackTransaction(std::uint64_t) = 0;
    virtual void log(const LogRecord&) = 0;          // audit/command log (§1.7)
    virtual void emit(std::string_view topic, json::Value payload) = 0;   // events (§3.4)
};
```

`execute`: resolve name (unknown → `command.unknown` with up to 3 closest
names in the hint); parse `@version` (unknown version → `command.version`
listing available versions); validate args with the command's schema (all
problems, as `command.invalid_args` whose `details` are the schema errors,
paths relative to the args object); for `Edit` commands wrap in a single-step
transaction; run; encode the result through the result schema (a handler
returning a value that fails its own result schema is a bug: `command.
bad_result`, logged loudly, and a test failure in examples); log; return.
Handler errors pass through unchanged (their own codes), with `path`
prefixed by `/args` when it is an args path.

### 1.5 `batch`

Registered by the library, `Kind::Edit`, `McpTool`.

```json
{
  "label": "Bass patch: darker, shorter release",
  "steps": [
    { "command": "plugin.load", "args": { "plugin": "com.suppleaudio.probe-synth" }, "as": "synth" },
    { "command": "patch.merge", "args": { "instance": "${synth.instance}", "patch": { "params": { "filter.cutoff": "800 Hz" } } } },
    { "command": "render", "args": { "notation": "1: C2:q r:q C2:q r:q", "analyze": "summary" }, "as": "r1" }
  ]
}
```

- Steps run in order inside **one transaction** (one undo step in the
  engine). If any step fails, the transaction rolls back and the error is
  `batch.step_failed` with `path` `/steps/<i>` (and the step's own error in
  `details`, its paths prefixed with `/steps/<i>/args`).
- **Step references:** a string that is exactly `"${<name>.<path>}"` is
  replaced, before that step is validated, by the value at `<path>` (dot
  separated keys / array indices) in the result of the earlier step with
  `"as": "<name>"`. Unknown names/paths → `batch.bad_reference` with the
  available names. References are the only templating; there is no
  expression language.
- All steps are resolvable before running (`command` names checked first,
  so a misspelled step fails before anything runs).
- Result: `{"label": …, "results": [ … one per step … ]}`.
- `Query` commands are allowed inside batches (they see earlier steps'
  effects). `Action` commands are allowed only if the host declares them
  transaction-safe (the sandbox's `render`/`analyze` are).
- Limits: ≤ 256 steps.

### 1.6 Registry export

`exportJson()` →

```json
{
  "schema": "supple.command-registry/1.0",
  "product": "<name from product.h>", "version": "<version>",
  "commands": {
    "patch.merge@1": { "name": "patch.merge", "version": 1, "kind": "edit", "exposure": "mcp-tool",
                        "summary": "…", "description": "…", "tags": [...],
                        "args": { JSON Schema }, "result": { JSON Schema },
                        "errors": [ {"code", "when", "hint"} ], "examples": [ … ] }
  }
}
```

Generators (each a function in the library, deterministic output, committed
results checked by a drift test):

- `mcpToolList(registry)` — MCP tool definitions (§4.2).
- `markdownReference(registry)` → `skill/generated/commands.md`: per command,
  heading, summary, description, an args table (field, type, unit, range,
  default, description), result shape, errors with hints, examples as JSON.
- `luaStubs(registry)` → LuaLS annotations (Phase 4).

### 1.7 Command log

Every executed invocation (including each batch step) produces a
`LogRecord {seq, time_ms (host clock; tests use a fixed clock), origin,
command, version, args (after reference resolution), ok, error code,
result digest (first 200 chars), duration_us}`. The host writes it (the
sandbox to an NDJSON file per session; the engine into the session history).

### 1.8 Examples are tests

`runExamples(const Registry&, FixtureFactory)` (test helper in the library)
executes every example against `factory(example.fixture)` and checks the
result matches (`"<any>"` wildcards allowed); every Example of every
non-Internal command must pass. Each application registers its fixtures.

### 1.9 Error codes owned by the bus

`command.unknown`, `command.version`, `command.invalid_args`,
`command.bad_result`, `batch.step_failed`, `batch.bad_reference`,
`batch.too_many_steps`, `batch.not_transactional` (an Action that is not
transaction-safe inside a batch), `rpc.*` (§3), `mcp.*` (§4).

---

## 2. Conventions for command designers

- **Units:** numeric args with physical meaning declare `.unit()` and accept
  unit strings.
- **References:** object references accept an ID string or a selector
  object; the `Ref` schema type (in command-bus) resolves via a host service
  and errors with candidates on ambiguity.
- **Results** are compact: return IDs and the few fields an agent needs next,
  not whole objects; offer `detail` args (`summary` / `full`) where outputs
  can be large.
- **Idempotence:** `set`/`merge` commands are idempotent; `add` returns the
  new object's ID.
- **Descriptions** say when to use the command, what it does not do, and
  which command to use instead.

---

## 3. Control protocol (`libs/control-protocol`)

- **Transport:** Unix domain stream socket at
  `$XDG_RUNTIME_DIR/<slug>/<instance>.sock` (directory 0700, socket 0600).
  Framing: one JSON value per line (UTF-8, `\n`-terminated, compact JSON; no
  embedded newlines), max 16 MiB per message.
- **JSON-RPC 2.0.** Request `{"jsonrpc":"2.0","id":…,"method":"<command>","params":{…}}`;
  `method` is a command name (optionally `@version`). Notifications from
  clients (no `id`) are rejected with no response (logged). JSON-RPC array
  batches are rejected with `-32600` and a hint to use the atomic `batch`
  command.
- **Errors:** `-32700` parse error, `-32600` invalid request, `-32601`
  method not found, `-32602` invalid params, `-32000` command error. `data`
  is always the `supple::Error` JSON (code, message, path, hint, details).
- **Handshake:** `rpc.hello {"client": "...", "agent": "...", "protocol": 1}`
  → `{"server": "<slug>", "product": "<name>", "version": "...",
  "protocol": 1, "commands": N}`; required before other methods. Sets the
  connection's `Origin` (type `Rpc`, or `Mcp` when the MCP proxy connects).
- **Events:** `rpc.subscribe {"topics": ["session.changed", …], "filter": {…}}`
  → `{"subscription": "sub_1"}`; server pushes
  `{"jsonrpc":"2.0","method":"event","params":{"subscription":"sub_1","topic":…,"payload":…}}`.
  `rpc.unsubscribe {"subscription": …}`. Per-connection bounded outbound
  queue; on overflow the connection gets one `event.dropped` notification
  with a count, never blocking the model thread.
- **Threading:** a small I/O thread per server accepts connections and
  frames messages; requests are posted to the model thread's queue and
  answered in order per connection.

---

## 4. MCP adapter (`mcp/`)

- **Protocol:** MCP over stdio (JSON-RPC 2.0, newline-delimited), protocol
  version negotiation per the MCP spec: support `2025-06-18` and newer
  revisions that are backward compatible; reply with the client's requested
  version if supported, else the newest supported. Capabilities: `tools`
  (no `listChanged` in Phase 2). `ping` supported. `logging`/`resources`
  later.
- **Deployments:** in-process (`McpServer` wraps a `Dispatcher` directly —
  the sandbox CLI `--mcp` mode) and proxy (`<slug>-mcp` forwarding to a
  control-protocol socket).

### 4.1 Tool naming

Tool name = command name with `.` replaced by `_` (`patch.merge` →
`patch_merge`); `@version` is not exposed (tools always call the latest
version). Generic tools: `describe` (→ `registry.describe`/`registry.list`),
`exec` (→ one invocation or a batch: `{"command", "args"}` or
`{"batch": {...}}`), and in the engine, `query`.

### 4.2 Tool definitions

For every `McpTool` command: `name`, `title` (summary), `description`
(summary + description + a one-line pointer to errors/examples via
`describe`), `inputSchema` (args JSON Schema, with `x-unit` hints folded into
field descriptions like "(Hz; accepts \"1.2 kHz\")" because clients ignore
unknown keywords), `outputSchema` (result JSON Schema), `annotations`
(`readOnlyHint` for Query, `destructiveHint: false` for Edit — undoable —
and `idempotentHint` where true).

### 4.3 Calls and results

`tools/call` → dispatcher `execute` with `Origin{Mcp, client name from
initialize, agent}`. Success: `structuredContent` = result JSON and
`content: [{"type":"text","text": <compact JSON of the result>}]`. Failure:
`isError: true`, `content: [{"type":"text","text": <error rendered as
"code at path: message\nHint: hint" plus details>}]`, `structuredContent` =
the Error JSON. Protocol-level problems (unknown tool) are JSON-RPC errors
per MCP.

### 4.4 Tests

A scripted MCP client (C++ test or Python tooling in `tests/integration/`)
runs initialize → tools/list → a sequence of calls against the sandbox, and
checks results; its transcript (requests/responses, pretty-printed) is the
Phase 2 demo committed to `docs/demos/`.
