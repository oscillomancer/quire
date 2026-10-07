#include "conversion.h"

// Lua is built as C++ (third_party/CMakeLists.txt): no extern "C".
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace supple::lua {
namespace {

char kNullSentinel;

// What the schema at this position asks for. Multi is the unit form
// (["number", "string"] with anyOf): a plain scalar either way.
enum class Kind : std::uint8_t { Any, Object, Array, String, Integer, Number, Boolean, Multi };

Kind kindFromType(const json::Value& type) {
    if (type.isArray()) return Kind::Multi;
    if (!type.isString()) return Kind::Any;
    const auto& name = type.asString();
    if (name == "object") return Kind::Object;
    if (name == "array") return Kind::Array;
    if (name == "string") return Kind::String;
    if (name == "integer") return Kind::Integer;
    if (name == "number") return Kind::Number;
    if (name == "boolean") return Kind::Boolean;
    return Kind::Any;
}

// A schema position: the node, the root its references resolve against, and
// the nearest enclosing node with its own $defs. schema::named puts a
// definition beside its reference ({"$ref": "#/$defs/x", "$defs": {...}}),
// which may sit below the root; a reference the root cannot resolve is tried
// there. A null node says nothing about the value.
struct Node {
    const json::Value* schema = nullptr;
    const json::Value* root = nullptr;
    const json::Value* scope = nullptr;
};

constexpr int kMaxReferenceHops = 32;  // a $ref chain longer than this is a cycle

Node enter(Node node) {
    if (node.schema && node.schema->isObject())
        if (const auto* defs = node.schema->find("$defs"); defs && defs->isObject())
            node.scope = node.schema;
    return node;
}

// Follows $ref (a JSON Pointer fragment) to the schema it names. A reference
// that does not resolve, or a cycle, says nothing about the value.
Node resolve(Node node) {
    node = enter(node);
    for (int hop = 0; node.schema && node.schema->isObject(); ++hop) {
        const auto* ref = node.schema->find("$ref");
        if (!ref || !ref->isString()) return node;
        if (hop == kMaxReferenceHops) return {nullptr, node.root, node.scope};
        const json::Value* target =
            node.root ? json::resolveFragment(*node.root, ref->asString()) : nullptr;
        if (!target && node.scope) target = json::resolveFragment(*node.scope, ref->asString());
        if (!target) return {nullptr, node.root, node.scope};
        node = enter({target, node.root, node.scope});
    }
    return node;
}

Node child(const Node& parent, const json::Value* schema) {
    return {schema, parent.root, parent.scope};
}

// The kind of a Lua value at `index` as the schema sees it: a table is an
// object unless the schema names an array (or a sequence with no schema).
Kind kindOf(lua_State* L, int index, const Node& at) {
    const Node node = resolve(at);
    const json::Value* s = node.schema;
    if (s && s->isObject()) {
        if (const auto* choices = s->find("oneOf"); choices && choices->isArray()) {
            // Prefer the alternative matching the value (a reference is an ID
            // string or a selector object).
            const Kind table = lua_istable(L, index) ? Kind::Object : Kind::Any;
            for (const auto& alternative : choices->asArray()) {
                const json::Value* alt = resolve(child(node, &alternative)).schema;
                const auto* type = alt && alt->isObject() ? alt->find("type") : nullptr;
                Kind kind = type ? kindFromType(*type) : Kind::Any;
                if (table == Kind::Object && kind == Kind::Object) return Kind::Object;
                if (table == Kind::Object && kind == Kind::Array) return Kind::Array;
            }
            return table;
        }
        if (const auto* type = s->find("type")) return kindFromType(*type);
        if (s->find("anyOf")) return lua_istable(L, index) ? Kind::Object : Kind::Any;
    }
    if (!lua_istable(L, index)) return Kind::Any;
    // No schema, or a schema that says nothing about the type: a non-empty
    // sequence is an array, anything else is an object.
    return lua_rawlen(L, index) > 0 ? Kind::Array : Kind::Object;
}

// Every key is an integer in 1..len (a Lua sequence) with no holes.
bool sequenceOnly(lua_State* L, int index, std::size_t len) {
    const int table = lua_absindex(L, index);
    std::size_t seen = 0;
    bool bad = false;
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        ++seen;
        if (!lua_isinteger(L, -2))
            bad = true;
        else {
            const auto key = lua_tointeger(L, -2);
            if (key < 1 || static_cast<std::size_t>(key) > len) bad = true;
        }
        lua_pop(L, 1);
    }
    return !bad && seen == len;
}

Node memberSchema(const Node& at, const std::string& key) {
    const Node node = resolve(at);
    const json::Value* s = node.schema;
    if (s && s->isObject()) {
        if (const auto* properties = s->find("properties"); properties && properties->isObject())
            if (const auto* member = properties->find(key); member && member->isObject())
                return resolve(child(node, member));
        if (const auto* extra = s->find("additionalProperties"); extra && extra->isObject())
            return resolve(child(node, extra));
    }
    return child(node, nullptr);
}

Node itemSchema(const Node& at) {
    const Node node = resolve(at);
    const json::Value* s = node.schema;
    if (s && s->isObject())
        if (const auto* items = s->find("items"); items && items->isObject())
            return resolve(child(node, items));
    return child(node, nullptr);
}

// What one conversion may produce. The values are built outside the Lua
// allocator and after a script's instruction limit, and a table referenced
// twice is converted twice, so depth alone cannot bound the work.
struct Budget {
    static constexpr std::size_t kMaxValues = 1'000'000;
    static constexpr std::size_t kMaxBytes = 16u << 20;  // strings and keys
    std::size_t values = 0, bytes = 0;
    Status take(std::size_t valueCount, std::size_t byteCount, const std::string& path) {
        values += valueCount;
        bytes += byteCount;
        if (values > kMaxValues || bytes > kMaxBytes)
            return Error("script.value_too_large",
                         "the value exceeds the JSON conversion limit of 1,000,000 values and "
                         "16 MiB of text",
                         path, "return or pass less data (a shared table counts once per use)");
        return {};
    }
};

Result<json::Value> tableToJson(lua_State* L, int index, const Node& schema,
                                const std::string& path, int depth, Budget& budget);

Result<json::Value> valueToJson(lua_State* L, int index, const Node& schema,
                                const std::string& path, int depth, Budget& budget) {
    if (depth > 64 || !lua_checkstack(L, 4))
        return Error("script.value", "table nesting exceeds the JSON conversion limit", path,
                     "use an acyclic table with at most 64 levels");
    SUPPLE_TRY(budget.take(1, 0, path));
    switch (lua_type(L, index)) {
        case LUA_TNIL:
            return json::Value();
        case LUA_TBOOLEAN:
            return json::Value(static_cast<bool>(lua_toboolean(L, index)));
        case LUA_TNUMBER: {
            if (lua_isinteger(L, index))
                return json::Value(static_cast<long long>(lua_tointeger(L, index)));
            const double number = static_cast<double>(lua_tonumber(L, index));
            if (!std::isfinite(number))
                return Error("script.value", "non-finite numbers have no JSON form", path,
                             "use a finite number");
            return json::Value(number);
        }
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            SUPPLE_TRY(budget.take(0, length, path));
            return json::Value(std::string(text, length));
        }
        case LUA_TTABLE:
            return tableToJson(L, index, schema, path, depth, budget);
        case LUA_TLIGHTUSERDATA:
            if (isNull(L, index)) return json::Value();
            break;
        default:
            break;
    }
    return Error(
        "script.value",
        std::string("a ") + lua_typename(L, lua_type(L, index)) + " value has no JSON form", path,
        "pass a string, number, boolean, table (or q.null)");
}

Result<json::Value> tableToJson(lua_State* L, int index, const Node& schema,
                                const std::string& path, int depth, Budget& budget) {
    const int table = lua_absindex(L, index);
    const Kind kind = kindOf(L, table, schema);
    const std::size_t length = lua_rawlen(L, table);
    if (kind == Kind::Array) {
        if (!sequenceOnly(L, table, length))
            return Error("script.table",
                         "expected a sequence for " + (path.empty() ? "arguments" : path), path,
                         "use {value, value, ...} (a table with string keys is an object)");
        json::Value out = json::Value::array();
        const Node items = itemSchema(schema);
        for (std::size_t i = 1; i <= length; ++i) {
            lua_rawgeti(L, table, static_cast<lua_Integer>(i));
            auto item =
                valueToJson(L, -1, items, path + "/" + std::to_string(i - 1), depth + 1, budget);
            lua_pop(L, 1);
            if (!item) return item.error();
            out.push_back(std::move(*item));
        }
        return out;
    }
    json::Value out = json::Value::object();
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
            std::size_t keyLength = 0;
            const char* keyText = lua_tolstring(L, -2, &keyLength);
            key.assign(keyText, keyLength);
        } else if (lua_isinteger(L, -2))
            // An integer key in an object position is passed through as its
            // decimal name: the bus then reports it as an unknown member,
            // which is more useful than refusing it here.
            key = std::to_string(lua_tointeger(L, -2));
        else if (lua_type(L, -2) == LUA_TNUMBER)
            return Error("script.table", "object keys must be strings or integers", path,
                         "use {\"name\": value} with string keys");
        else
            return Error("script.table", "object keys must be strings", path,
                         "use {\"name\": value} with string keys");
        if (auto taken = budget.take(0, key.size(), path); !taken) {
            lua_pop(L, 2);  // the value and the key: lua_next is not resumed
            return taken.error();
        }
        const Node member = memberSchema(schema, key);
        auto value = valueToJson(L, -1, member, json::pointerJoin(path, key), depth + 1, budget);
        lua_pop(L, 1);
        if (!value) return value.error();
        out[key] = std::move(*value);
    }
    return out;
}

}  // namespace

void pushNull(lua_State* L) { lua_pushlightuserdata(L, &kNullSentinel); }
bool isNull(lua_State* L, int index) {
    return lua_type(L, index) == LUA_TLIGHTUSERDATA &&
           lua_touserdata(L, index) == static_cast<void*>(&kNullSentinel);
}

namespace {
void pushJsonAt(lua_State* L, const json::Value& value, int depth) {
    if (depth > kMaxPushDepth) {
        pushError(L, Error("script.value",
                           "the JSON value nests deeper than " + std::to_string(kMaxPushDepth) +
                               " levels",
                           {}, "pass a flatter value"));
        lua_error(L);
    }
    luaL_checkstack(L, 3, "converting a JSON value");
    switch (value.type()) {
        case json::Type::Null:
            pushNull(L);
            return;
        case json::Type::Bool:
            lua_pushboolean(L, value.asBool());
            return;
        case json::Type::Int:
            lua_pushinteger(L, static_cast<lua_Integer>(value.asInt()));
            return;
        case json::Type::Double:
            lua_pushnumber(L, static_cast<lua_Number>(value.asDouble()));
            return;
        case json::Type::String:
            lua_pushlstring(L, value.asString().data(), value.asString().size());
            return;
        case json::Type::Array: {
            lua_createtable(L, static_cast<int>(std::min<std::size_t>(value.size(), INT_MAX)), 0);
            const int table = lua_gettop(L);
            lua_Integer position = 1;
            for (const auto& item : value.asArray()) {
                pushJsonAt(L, item, depth + 1);
                lua_rawseti(L, table, position++);
            }
            return;
        }
        case json::Type::Object: {
            lua_createtable(L, 0, static_cast<int>(std::min<std::size_t>(value.size(), INT_MAX)));
            const int table = lua_gettop(L);
            for (const auto& member : value.asObject()) {
                lua_pushlstring(L, member.key.data(), member.key.size());
                pushJsonAt(L, member.value, depth + 1);
                lua_rawset(L, table);
            }
            return;
        }
    }
}

void pushErrorAt(lua_State* L, const Error& error, int depth) {
    luaL_checkstack(L, 4, "building an error table");
    lua_createtable(L, 0, 5);
    const int table = lua_gettop(L);
    lua_pushlstring(L, error.code.data(), error.code.size());
    lua_setfield(L, table, "code");
    lua_pushlstring(L, error.message.data(), error.message.size());
    lua_setfield(L, table, "message");
    lua_pushlstring(L, error.path.data(), error.path.size());
    lua_setfield(L, table, "path");
    lua_pushlstring(L, error.hint.data(), error.hint.size());
    lua_setfield(L, table, "hint");
    // Details deeper than a script could usefully read are left out rather
    // than allowed to exhaust the stack.
    const bool keep = depth < kMaxPushDepth;
    lua_createtable(L, keep ? static_cast<int>(error.details.size()) : 0, 0);
    const int details = lua_gettop(L);
    lua_Integer position = 1;
    if (keep)
        for (const auto& detail : error.details) {
            pushErrorAt(L, detail, depth + 1);
            lua_rawseti(L, details, position++);
        }
    lua_setfield(L, table, "details");
}
}  // namespace

void pushJson(lua_State* L, const json::Value& value) { pushJsonAt(L, value, 0); }

Result<json::Value> toJson(lua_State* L, int index, const json::Value* schema,
                           const std::string& path) {
    Budget budget;
    return valueToJson(L, index, Node{schema, schema, nullptr}, path, 0, budget);
}

void pushError(lua_State* L, const Error& error) { pushErrorAt(L, error, 0); }

namespace {
// Pushes t[name] without metamethods: an error table's __index must not run
// while the host decodes it.
int rawField(lua_State* L, int table, const char* name) {
    lua_pushstring(L, name);
    return lua_rawget(L, table);
}

std::string stringAt(lua_State* L, int index) {
    std::size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return text ? std::string(text, length) : std::string();
}

// Runs in protected mode (errorFromLua): reads only raw fields and strings,
// so no Lua code runs.
Error errorFromLuaAt(lua_State* L, int index, int depth) {
    if (depth > 64 || !lua_checkstack(L, 4))
        return Error("script.error", "error details exceed the nesting limit", {},
                     "use an acyclic error table with at most 64 levels");
    index = lua_absindex(L, index);
    if (lua_type(L, index) == LUA_TTABLE) {
        Error error;
        if (rawField(L, index, "code") == LUA_TSTRING) error.code = stringAt(L, -1);
        lua_pop(L, 1);
        if (rawField(L, index, "message") == LUA_TSTRING) error.message = stringAt(L, -1);
        lua_pop(L, 1);
        if (rawField(L, index, "path") == LUA_TSTRING) error.path = stringAt(L, -1);
        lua_pop(L, 1);
        if (rawField(L, index, "hint") == LUA_TSTRING) error.hint = stringAt(L, -1);
        lua_pop(L, 1);
        if (rawField(L, index, "details") == LUA_TTABLE) {
            const std::size_t count = lua_rawlen(L, -1);
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
                error.details.push_back(errorFromLuaAt(L, -1, depth + 1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        if (!error.code.empty() || !error.message.empty()) {
            if (error.code.empty()) error.code = "script.error";
            return error;
        }
        return Error("script.error", "the script raised a table error with no code or message");
    }
    if (lua_type(L, index) == LUA_TSTRING) return Error("script.error", stringAt(L, index));
    return Error("script.error", "the script raised a non-string, non-table error value");
}

}  // namespace

Error errorFromLua(lua_State* L, int index) {
    Error out("script.error", "the script's error could not be read (the state is out of memory)",
              {}, "make the script hold less data");
    const int top = lua_gettop(L);
    if (!lua_checkstack(L, 1)) return out;
    lua_pushvalue(L, index);
    auto decode = [&out](lua_State* S) -> int {
        out = errorFromLuaAt(S, 1, 0);
        return 0;
    };
    (void)protectedCall(L, 1, 0, decode);
    lua_settop(L, top);
    return out;
}

}  // namespace supple::lua
