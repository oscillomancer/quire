// JSON ⇄ Lua conversion for the scripting bindings (ADR-0006).
//
// Objects become tables with string keys and arrays become sequences, which
// Lua cannot tell apart on its own: an empty table is an object unless the
// command's argument schema at that path says array. Numbers that came from
// JSON integers stay Lua integers.
#pragma once

// Lua is built as C++ (third_party/CMakeLists.txt): no extern "C".
#include <lua.h>

#include <string>
#include <type_traits>

#include "supple/base/result.h"
#include "supple/json/json.h"

namespace supple::lua {

// Runs `body` (an `int(lua_State*)` returning its result count, like a
// lua_CFunction) in protected mode on the `nargs` values at the top of the
// stack, which it sees at 1..nargs. Anything it raises (a memory or stack
// error, a structured error) is caught: the status is returned and, on
// failure, the error value is on top as with lua_pcall, except for LUA_ERRMEM
// when the stack could not even grow for the call (the arguments are popped
// and nothing is pushed; callers restore their own top). On success
// `nresults` results are left. The host never sees a raise outside a protected
// call: an unprotected one would end in Lua's panic handler.
template <class Body>
int protectedCall(lua_State* L, int nargs, int nresults, Body& body) {
    if (!lua_checkstack(L, 2 + (nresults > 0 ? nresults : 0))) {
        lua_pop(L, nargs);
        return LUA_ERRMEM;
    }
    const int first = lua_gettop(L) - nargs + 1;
    lua_pushcfunction(L, [](lua_State* S) -> int {
        auto* call = static_cast<Body*>(lua_touserdata(S, 1));
        lua_remove(S, 1);
        return (*call)(S);
    });
    lua_insert(L, first);
    lua_pushlightuserdata(L, static_cast<void*>(&body));
    lua_insert(L, first + 1);
    return lua_pcall(L, nargs + 1, nresults, 0);
}

// The value a JSON null becomes. It is a unique light userdata because Lua
// tables cannot hold nil: `q.null` is that value, and it converts back to null.
void pushNull(lua_State* L);
bool isNull(lua_State* L, int index);

// JSON → Lua. Raises (call it in protected mode) when the value nests deeper
// than kMaxPushDepth or the state runs out of stack or memory.
inline constexpr int kMaxPushDepth = 256;  // the JSON parser's default nesting limit
void pushJson(lua_State* L, const json::Value& value);

// Lua → JSON, guided by the JSON Schema of the argument at this position
// (`schema` may be null: the value is then inferred, a non-empty sequence
// being an array and anything else an object). `schema` is the root that its
// `$ref`s ("#/$defs/name") resolve against. `path` is the JSON Pointer of the
// value inside the arguments object, for error paths.
Result<json::Value> toJson(lua_State* L, int index, const json::Value* schema,
                           const std::string& path);

// The error table a script sees: {code, message, path, hint, details}.
// Raises on memory or stack exhaustion: call it in protected mode.
void pushError(lua_State* L, const Error& error);
// The reverse: an error value raised by Lua (a table from pushError, a string
// from error(), anything else) as a supple::Error. Safe outside a protected
// call: it reads the fields raw (no metamethod runs) inside its own protected
// call, and falls back to a generic error when even that fails.
Error errorFromLua(lua_State* L, int index);

}  // namespace supple::lua
