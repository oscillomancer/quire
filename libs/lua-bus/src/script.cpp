#include "supple/lua/script.h"

#include <lua.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>

#include "conversion.h"

namespace supple::lua {

// One script's Lua state: sandbox, generated bindings, budget and memory
// accounting, log, subscriptions. Everything here is model-thread only.
struct ScriptImpl {
    const cmd::Registry* registry = nullptr;
    cmd::Dispatcher* dispatcher = nullptr;
    ScriptOptions options;
    lua_State* L = nullptr;
    std::uint64_t usedBytes = 0;
    bool memoryExceeded = false;
    bool budgetSpent = false;
    std::chrono::steady_clock::time_point deadline{};
    std::uint64_t instructions = 0;
    int messageHandlerRef = LUA_NOREF;
    bool running = false;
    // Log ring: log holds at most logLimitLines lines, logStart its oldest.
    std::vector<std::string> log;
    std::size_t logStart = 0;
    std::uint64_t logTotal = 0;
    std::string chunkName;

    struct Subscription {
        std::uint64_t id = 0;
        std::string topic;
        int ref = LUA_NOREF;
        json::Value filter;
    };
    std::vector<Subscription> subscriptions;
    std::uint64_t nextSubscription = 1;

    cmd::Origin origin() const { return {cmd::Origin::Type::Lua, options.name, {}}; }

    Result<json::Value> invoke(std::string_view command, json::Value args) {
        if (!dispatcher)
            return Error("script.unavailable", "the script has no command dispatcher", {},
                         "run scripts through a Runtime that has a dispatcher");
        return dispatcher->execute(
            cmd::Invocation{std::string(command), std::move(args), origin()});
    }
    void appendLog(std::string line);
    std::vector<std::string> lines() const;
    Status ensureState();
};

namespace {

constexpr const char* kErrorHint = "the bus reports the call that failed; see registry.describe";

// --- memory -------------------------------------------------------------------

void* luaAllocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* impl = static_cast<ScriptImpl*>(ud);
    if (nsize == 0) {
        if (ptr) impl->usedBytes -= osize;
        std::free(ptr);
        return nullptr;
    }
    const std::uint64_t used = ptr ? impl->usedBytes - osize : impl->usedBytes;
    if (used + nsize > impl->options.memoryLimitBytes) {
        impl->memoryExceeded = true;
        return nullptr;
    }
    void* out = std::realloc(ptr, nsize);
    if (!out) {
        impl->memoryExceeded = true;
        return nullptr;
    }
    impl->usedBytes = used + nsize;
    return out;
}

ScriptImpl* implOf(lua_State* L) { return *static_cast<ScriptImpl**>(lua_getextraspace(L)); }

// --- budget -------------------------------------------------------------------

void budgetHook(lua_State* L, lua_Debug*) {
    ScriptImpl* impl = implOf(L);
    impl->instructions += impl->options.hookCount;
    if (std::chrono::steady_clock::now() >= impl->deadline) {
        impl->budgetSpent = true;
        luaL_error(L, "the script spent its instruction budget");
    }
}

void startBudget(lua_State* L, ScriptImpl& impl, double budgetMs) {
    impl.instructions = 0;
    const auto budget = std::chrono::duration<double, std::milli>(budgetMs);
    impl.deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(budget);
    lua_sethook(L, budgetHook, LUA_MASKCOUNT, static_cast<int>(impl.options.hookCount));
}

void stopBudget(lua_State* L) { lua_sethook(L, nullptr, 0, 0); }

// Keeps structured errors intact and adds a traceback to everything else.
int messageHandler(lua_State* L) {
    if (lua_type(L, 1) == LUA_TTABLE) return 1;
    const char* text = lua_tostring(L, 1);
    luaL_traceback(L, L, text, 1);
    return 1;
}

// Calls the function at (top - nargs) through the message handler. On failure
// the error value is on top; the handler itself is always popped.
int callProtected(lua_State* L, ScriptImpl& impl, int nargs, int nresults) {
    const int function = lua_gettop(L) - nargs;  // the function sits below its arguments
    lua_rawgeti(L, LUA_REGISTRYINDEX, impl.messageHandlerRef);
    lua_insert(L, function);
    const int status = lua_pcall(L, nargs, nresults, function);
    lua_remove(L, function);
    return status;
}

// --- raising ------------------------------------------------------------------

int raiseError(lua_State* L, const Error& error) {
    pushError(L, error);
    return lua_error(L);
}

Error unavailable() {
    return Error("script.unavailable", "the script has no command dispatcher", {},
                 "run scripts through a Runtime that has a dispatcher");
}

// The command a binding calls, by name (with an optional `@version`).
const cmd::CommandInfo* findInfo(ScriptImpl& impl, std::string_view name) {
    if (!impl.registry) return nullptr;
    const auto at = name.find('@');
    if (at == std::string_view::npos) return impl.registry->find(name);
    int version = 0;
    for (char ch : name.substr(at + 1)) {
        if (ch < '0' || ch > '9') return nullptr;
        version = version * 10 + (ch - '0');
    }
    return impl.registry->find(name.substr(0, at), version);
}

// Arguments of a call: a table or nothing. Conversion errors carry the same
// path shape as the bus's own argument errors ("/args/...").
Result<json::Value> argumentsFrom(lua_State* L, ScriptImpl& impl, std::string_view name,
                                  int index) {
    if (lua_gettop(L) < index || lua_isnil(L, index)) return json::Value::object();
    if (!lua_istable(L, index))
        return Error("script.arguments", "command arguments must be a table", "/args",
                     "call with a table of arguments, e.g. q.track.add{name = \"Bass\"}");
    const cmd::CommandInfo* info = findInfo(impl, name);
    auto converted = toJson(L, index, info ? &info->argsSchema : nullptr, "");
    if (!converted) {
        Error error = std::move(converted).error();
        if (error.path.empty() || error.path.front() == '/') error.path = "/args" + error.path;
        return error;
    }
    return converted;
}

// Runs one command and returns its result as Lua values (1 result) or raises.
Result<json::Value> runCommand(lua_State* L, ScriptImpl& impl, std::string_view name, int index) {
    auto args = argumentsFrom(L, impl, name, index);
    if (!args) return args.error();
    return impl.invoke(name, std::move(*args));
}

// --- generated bindings -------------------------------------------------------

int bindingCall(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    const char* name = lua_tostring(L, lua_upvalueindex(2));
    if (!name) return raiseError(L, Error("script.binding", "the binding lost its command"));
    auto result = runCommand(L, *impl, name, 1);
    if (!result) return raiseError(L, result.error());
    pushJson(L, *result);
    return 1;
}

// q.run(name, args): any registered command, including the ones whose names
// collide with a binding (`batch`) or are not Lua identifiers (`branch.render-ab`).
int genericRun(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    if (!lua_isstring(L, 1)) return luaL_argerror(L, 1, "expected a command name");
    std::string name = lua_tostring(L, 1);
    auto result = runCommand(L, *impl, name, 2);
    if (!result) return raiseError(L, result.error());
    pushJson(L, *result);
    return 1;
}

// q.batch(label, function() ... end): the calls inside the body are one
// transaction, so they are one undo step, and an error inside rolls it back
// and is re-raised unchanged.
int batchCall(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    if (!impl->dispatcher) return raiseError(L, unavailable());
    if (!lua_isstring(L, 1)) return luaL_argerror(L, 1, "expected a label for the batch");
    if (!lua_isfunction(L, 2)) return luaL_argerror(L, 2, "expected the batch body function");
    const std::string label = lua_tostring(L, 1);
    const int base = lua_gettop(L);
    bool raised = false;
    auto body = [&]() -> Result<json::Value> {
        lua_pushvalue(L, 2);
        if (callProtected(L, *impl, 0, 0) != LUA_OK) {
            raised = true;
            return errorFromLua(L, -1);
        }
        return json::Value();
    };
    auto result = impl->dispatcher->transact(label, impl->origin(), body);
    if (result) return 0;
    if (raised) {
        lua_settop(L, base + 1);  // the error value the body raised
        return lua_error(L);
    }
    return raiseError(L, result.error());
}

// --- subscriptions ------------------------------------------------------------

bool topicMatches(const std::string& subscription, std::string_view topic) {
    return subscription == "*" || subscription == topic;
}

// The same filter as the control protocol: every member of the filter must be
// present in the payload with the same value.
bool filterMatches(const json::Value& filter, const json::Value& payload) {
    if (!filter.isObject() || filter.asObject().empty()) return true;
    if (!payload.isObject()) return false;
    for (const auto& member : filter.asObject()) {
        const json::Value* actual = payload.find(member.key);
        if (!actual || *actual != member.value) return false;
    }
    return true;
}

// q.on(topic, function(payload, topic) ... end [, filter]) -> handle
int onCall(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    if (!lua_isstring(L, 1)) return luaL_argerror(L, 1, "expected a topic (\"*\" for all)");
    if (!lua_isfunction(L, 2)) return luaL_argerror(L, 2, "expected a handler function");
    json::Value filter = json::Value::object();
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        if (!lua_istable(L, 3)) return luaL_argerror(L, 3, "expected a filter table");
        auto converted = toJson(L, 3, nullptr, "");
        if (!converted) {
            Error error = std::move(converted).error();
            if (error.path.empty() || error.path.front() == '/') error.path = "/args" + error.path;
            return raiseError(L, error);
        }
        filter = std::move(*converted);
    }
    lua_pushvalue(L, 2);
    ScriptImpl::Subscription subscription;
    subscription.topic = lua_tostring(L, 1);
    subscription.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    subscription.filter = std::move(filter);
    subscription.id = impl->nextSubscription++;
    impl->subscriptions.push_back(std::move(subscription));
    lua_pushinteger(L, static_cast<lua_Integer>(impl->subscriptions.back().id));
    return 1;
}

int offCall(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    if (!lua_isinteger(L, 1)) return luaL_argerror(L, 1, "expected the handle q.on returned");
    const auto id = static_cast<std::uint64_t>(lua_tointeger(L, 1));
    auto& subs = impl->subscriptions;
    auto found = std::find_if(subs.begin(), subs.end(), [id](const auto& s) { return s.id == id; });
    if (found == subs.end()) return 0;  // already off
    luaL_unref(L, LUA_REGISTRYINDEX, found->ref);
    subs.erase(found);
    return 0;
}

// print and q.log: to the script's log, never to the host's stdout.
int logCall(lua_State* L) {
    ScriptImpl* impl = implOf(L);
    std::string line;
    const int count = lua_gettop(L);
    for (int i = 1; i <= count; ++i) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) line += '\t';
        if (text) line.append(text, length);
        lua_pop(L, 1);
    }
    impl->appendLog(std::move(line));
    return 0;
}

void pushBinding(lua_State* L, ScriptImpl& impl, const std::string& name) {
    lua_pushlightuserdata(L, &impl);
    lua_pushlstring(L, name.data(), name.size());
    lua_pushcclosure(L, bindingCall, 2);
}

// q.track.add -> the table q.track holds add. A name whose segment is already
// a function is left out (q.run reaches it); no such name exists today.
void installBinding(lua_State* L, ScriptImpl& impl, int qIndex, const std::string& name) {
    const int depth = lua_gettop(L);
    int current = qIndex;
    std::size_t start = 0;
    bool ok = true;
    while (true) {
        const auto dot = name.find('.', start);
        if (dot == std::string::npos) break;
        const std::string segment = name.substr(start, dot - start);
        lua_getfield(L, current, segment.c_str());
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, current, segment.c_str());
            current = lua_gettop(L);
        } else if (lua_istable(L, -1)) {
            current = lua_gettop(L);
        } else {
            lua_pop(L, 1);
            ok = false;
            break;
        }
        start = dot + 1;
    }
    if (ok) {
        const std::string leaf = name.substr(start);
        lua_getfield(L, current, leaf.c_str());
        const bool taken = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!taken) {
            pushBinding(L, impl, name);
            lua_setfield(L, current, leaf.c_str());
        }
    }
    lua_settop(L, depth);
}

void installBindings(lua_State* L, ScriptImpl& impl) {
    lua_newtable(L);
    const int qIndex = lua_gettop(L);
    pushNull(L);
    lua_setfield(L, qIndex, "null");
    lua_pushlstring(L, impl.options.name.data(), impl.options.name.size());
    lua_setfield(L, qIndex, "name");
    for (const auto& [key, function] :
         std::initializer_list<std::pair<const char*, lua_CFunction>>{{"run", genericRun},
                                                                      {"batch", batchCall},
                                                                      {"on", onCall},
                                                                      {"off", offCall},
                                                                      {"log", logCall}}) {
        lua_pushlightuserdata(L, &impl);
        lua_pushcclosure(L, function, 1);
        lua_setfield(L, qIndex, key);
    }
    for (auto* info :
         impl.registry ? impl.registry->all() : std::vector<const cmd::CommandInfo*>{}) {
        if (info->name == "batch") continue;                    // q.batch is the transaction above
        if (impl.registry->find(info->name) != info) continue;  // only the newest version
        installBinding(L, impl, qIndex, info->name);
    }
    lua_setglobal(L, "q");
}

// The sandbox (ADR-0006): the base library and string, table, math, utf8 and
// coroutine, and nothing else. io, os, debug and package are not even linked
// in, so a script cannot reach the filesystem, the clock or the process.
void openSandbox(lua_State* L, ScriptImpl& impl) {
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 1);
    // The base library's file access and the host's stdout are not part of a
    // script: no dofile/loadfile (no whitelisted library path yet), and print
    // writes to the script's log instead.
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushlightuserdata(L, &impl);
    lua_pushcclosure(L, logCall, 1);
    lua_setglobal(L, "print");
    installBindings(L, impl);
    impl.instructions = 0;
}

// A syntax error message is "lua:<name>:<line>: ...": lift the line out.
Error syntaxError(const std::string& chunkName, const std::string& message) {
    Error error("script.syntax", message, {},
                "fix the Lua syntax at that line, then run the script again");
    const std::string prefix = chunkName + ":";
    if (message.rfind(prefix, 0) == 0) {
        std::size_t end = prefix.size();
        while (end < message.size() && message[end] >= '0' && message[end] <= '9') ++end;
        if (end > prefix.size())
            error.loc = SourceLoc{std::stoi(message.substr(prefix.size(), end - prefix.size())), 1};
    }
    return error;
}

}  // namespace

// --- ScriptImpl ---------------------------------------------------------------------

void ScriptImpl::appendLog(std::string line) {
    const std::size_t limit = std::max<std::size_t>(options.logLimitLines, 1);
    if (log.size() < limit)
        log.push_back(std::move(line));
    else {
        log[logStart] = std::move(line);
        logStart = (logStart + 1) % limit;
    }
    ++logTotal;
}

std::vector<std::string> ScriptImpl::lines() const {
    std::vector<std::string> out;
    out.reserve(log.size());
    if (log.size() < std::max<std::size_t>(options.logLimitLines, 1)) {
        out = log;
        return out;
    }
    for (std::size_t i = 0; i < log.size(); ++i) out.push_back(log[(logStart + i) % log.size()]);
    return out;
}

Status ScriptImpl::ensureState() {
    if (L) return okStatus();
    memoryExceeded = false;
    L = lua_newstate(luaAllocate, this);
    if (!L)
        return Error("script.memory",
                     "the script state needs more than its " +
                         std::to_string(options.memoryLimitBytes) + " byte memory limit",
                     {}, "raise memory_limit_bytes for this script");
    *static_cast<ScriptImpl**>(lua_getextraspace(L)) = this;
    openSandbox(L, *this);
    lua_pushcfunction(L, messageHandler);
    messageHandlerRef = luaL_ref(L, LUA_REGISTRYINDEX);
    return okStatus();
}

// --- Script -------------------------------------------------------------------

Script::Script(const cmd::Registry* registry, cmd::Dispatcher* dispatcher, ScriptOptions options)
    : impl_(std::make_unique<ScriptImpl>()) {
    impl_->registry = registry;
    impl_->dispatcher = dispatcher;
    impl_->options = std::move(options);
    impl_->chunkName = impl_->options.name.empty() ? "script" : "lua:" + impl_->options.name;
}

// The commands of a host that attached its registry after this state existed:
// the new bindings are added to the live `q` and the calls reach the new
// dispatcher. Bindings are never removed (a command set does not shrink).
void Script::setCommands(const cmd::Registry* registry, cmd::Dispatcher* dispatcher) {
    impl_->registry = registry;
    impl_->dispatcher = dispatcher;
    if (impl_->L && registry) installBindings(impl_->L, *impl_);
}

Script::~Script() {
    if (impl_ && impl_->L) {
        for (auto& subscription : impl_->subscriptions)
            luaL_unref(impl_->L, LUA_REGISTRYINDEX, subscription.ref);
        lua_close(impl_->L);
    }
}

const ScriptOptions& Script::options() const { return impl_->options; }
cmd::Origin Script::origin() const { return impl_->origin(); }
std::vector<std::string> Script::log() const { return impl_->lines(); }
std::size_t Script::subscriptionCount() const { return impl_->subscriptions.size(); }

Result<ScriptResult> Script::run(std::string_view source, double budgetMs,
                                 const json::Value& args) {
    ScriptImpl& impl = *impl_;
    // One run at a time per state: a handler that ran its own script again
    // would replace the deadline and the hook under the outer run, and the
    // outer script would carry on without a budget.
    if (impl.running)
        return Error("script.busy", "the script is already running", {},
                     "a handler must not run the same script again");
    impl.running = true;
    struct Finished {
        bool& flag;
        ~Finished() { flag = false; }
    } finished{impl.running};
    if (auto ready = impl.ensureState(); !ready) return ready.error();
    lua_State* L = impl.L;
    impl.budgetSpent = false;
    impl.memoryExceeded = false;
    const std::uint64_t logFrom = impl.logTotal;
    const int base = lua_gettop(L);
    pushJson(L, args);
    lua_setglobal(L, "args");
    startBudget(L, impl, budgetMs >= 0 ? budgetMs : impl.options.defaultBudgetMs);
    const std::string chunk = "=" + impl.chunkName;  // '=': verbatim, not [string "…"]
    int status = luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t");
    if (status == LUA_OK) status = callProtected(L, impl, 0, 1);
    stopBudget(L);

    Error failure;
    if (status == LUA_OK && (impl.budgetSpent || impl.memoryExceeded))
        status = LUA_ERRRUN;  // the script caught the raise and carried on
    if (status == LUA_OK) {
        auto converted = toJson(L, -1, nullptr, "/result");
        if (!converted) {
            lua_settop(L, base);
            return converted.error();
        }
        ScriptResult result;
        result.result = std::move(*converted);
        result.script = impl.options.name;
        result.instructions = impl.instructions;
        std::vector<std::string> all = impl.lines();
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(impl.logTotal - logFrom, all.size()));
        result.log.assign(all.end() - static_cast<std::ptrdiff_t>(count), all.end());
        lua_settop(L, base);
        return result;
    }
    if (impl.budgetSpent)
        failure =
            Error("script.budget",
                  "the script spent its " +
                      std::to_string(budgetMs >= 0 ? budgetMs : impl.options.defaultBudgetMs) +
                      " ms budget",
                  {}, "raise budget_ms, or make the script do less between waits");
    else if (impl.memoryExceeded)
        failure = Error("script.memory",
                        "the script exceeded its " + std::to_string(impl.options.memoryLimitBytes) +
                            " byte memory cap",
                        {}, "raise memory_limit_bytes, or make the script hold less data");
    else if (status == LUA_ERRSYNTAX) {
        const char* text = lua_tostring(L, -1);
        failure = syntaxError(impl.chunkName, text ? text : "");
    } else
        failure = errorFromLua(L, -1);
    failure.hint = failure.hint.empty() ? kErrorHint : failure.hint;
    lua_settop(L, base);
    return failure;
}

void Script::publish(std::string_view topic, const json::Value& payload) {
    ScriptImpl& impl = *impl_;
    if (!impl.L || impl.subscriptions.empty()) return;
    lua_State* L = impl.L;
    std::vector<ScriptImpl::Subscription> matches;
    for (const auto& subscription : impl.subscriptions)
        if (topicMatches(subscription.topic, topic) && filterMatches(subscription.filter, payload))
            matches.push_back(subscription);
    for (const auto& subscription : matches) {
        const auto live = std::find_if(impl.subscriptions.begin(), impl.subscriptions.end(),
                                       [&](const auto& s) { return s.id == subscription.id; });
        if (live == impl.subscriptions.end()) continue;  // q.off inside an earlier handler
        const int base = lua_gettop(L);
        startBudget(L, impl, impl.options.defaultBudgetMs);
        lua_rawgeti(L, LUA_REGISTRYINDEX, subscription.ref);
        pushJson(L, payload);
        lua_pushlstring(L, topic.data(), topic.size());
        if (callProtected(L, impl, 2, 0) != LUA_OK) {
            Error error = errorFromLua(L, -1);
            impl.appendLog("handler for " + subscription.topic + " failed: " + error.code + ": " +
                           error.message);
        }
        stopBudget(L);
        lua_settop(L, base);
    }
}

}  // namespace supple::lua
