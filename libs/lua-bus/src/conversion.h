// JSON ⇄ Lua conversion for the scripting bindings (ADR-0006).
//
// Objects become tables with string keys and arrays become sequences, which
// Lua cannot tell apart on its own: an empty table is an object unless the
// command's argument schema at that path says array. Numbers that came from
// JSON integers stay Lua integers.
#pragma once

#include <string>

#include "supple/base/result.h"
#include "supple/json/json.h"

struct lua_State;

namespace supple::lua {

// The value a JSON null becomes. It is a unique light userdata because Lua
// tables cannot hold nil: `q.null` is that value, and it converts back to null.
void pushNull(lua_State* L);
bool isNull(lua_State* L, int index);

// JSON → Lua. `schema` is unused today (JSON is unambiguous); it is the hook
// for schema-driven representation changes.
void pushJson(lua_State* L, const json::Value& value);

// Lua → JSON, guided by the JSON Schema of the argument at this position
// (`schema` may be null: the value is then inferred, a non-empty sequence
// being an array and anything else an object). `path` is the JSON Pointer of
// the value inside the arguments object, for error paths.
Result<json::Value> toJson(lua_State* L, int index, const json::Value* schema,
                           const std::string& path);

// The error table a script sees: {code, message, path, hint, details}.
void pushError(lua_State* L, const Error& error);
// The reverse: an error value raised by Lua (a table from pushError, a string
// from error(), anything else) as a supple::Error.
Error errorFromLua(lua_State* L, int index);

}  // namespace supple::lua
