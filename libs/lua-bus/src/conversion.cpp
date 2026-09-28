#include "conversion.h"

#include <lua.hpp>

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

// A schema may be inline, a $ref into its own $defs (schema::named), or a
// choice (anyOf for units, oneOf for a variant or a reference).
const json::Value* resolve(const json::Value& schema) {
    if (!schema.isObject()) return &schema;
    if (const auto* ref = schema.find("$ref"); ref && ref->isString()) {
        const std::string& name = ref->asString();
        if (const auto* defs = schema.find("$defs"); defs && defs->isObject())
            if (const auto* target = defs->find(name); target && target->isObject())
                return resolve(*target);
        return &schema;
    }
    return &schema;
}

// The kind of a Lua value at `index` as the schema sees it: a table is an
// object unless the schema names an array (or a sequence with no schema).
Kind kindOf(lua_State* L, int index, const json::Value* schema) {
    const json::Value* s = schema ? resolve(*schema) : nullptr;
    if (s && s->isObject()) {
        if (const auto* choices = s->find("oneOf"); choices && choices->isArray()) {
            // Prefer the alternative matching the value (a reference is an ID
            // string or a selector object).
            const Kind table = lua_istable(L, index) ? Kind::Object : Kind::Any;
            for (const auto& alternative : choices->asArray()) {
                const json::Value* alt = resolve(alternative);
                const auto* type = alt->find("type");
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

const json::Value* memberSchema(const json::Value& schema, const std::string& key) {
    const json::Value* s = resolve(schema);
    if (s->isObject()) {
        if (const auto* properties = s->find("properties"); properties && properties->isObject())
            if (const auto* member = properties->find(key); member && member->isObject())
                return resolve(*member);
        if (const auto* extra = s->find("additionalProperties"); extra && extra->isObject())
            return resolve(*extra);
    }
    return nullptr;
}

const json::Value* itemSchema(const json::Value& schema) {
    const json::Value* s = resolve(schema);
    if (s->isObject())
        if (const auto* items = s->find("items"); items && items->isObject())
            return resolve(*items);
    return nullptr;
}

Result<json::Value> tableToJson(lua_State* L, int index, const json::Value* schema,
                                const std::string& path);

Result<json::Value> valueToJson(lua_State* L, int index, const json::Value* schema,
                                const std::string& path) {
    switch (lua_type(L, index)) {
        case LUA_TNIL:
            return json::Value();
        case LUA_TBOOLEAN:
            return json::Value(static_cast<bool>(lua_toboolean(L, index)));
        case LUA_TNUMBER:
            if (lua_isinteger(L, index))
                return json::Value(static_cast<long long>(lua_tointeger(L, index)));
            return json::Value(static_cast<double>(lua_tonumber(L, index)));
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            return json::Value(std::string(text, length));
        }
        case LUA_TTABLE:
            return tableToJson(L, index, schema, path);
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

Result<json::Value> tableToJson(lua_State* L, int index, const json::Value* schema,
                                const std::string& path) {
    const int table = lua_absindex(L, index);
    const Kind kind = kindOf(L, table, schema);
    const std::size_t length = lua_rawlen(L, table);
    if (kind == Kind::Array) {
        if (!sequenceOnly(L, table, length))
            return Error("script.table",
                         "expected a sequence for " + (path.empty() ? "arguments" : path), path,
                         "use {value, value, ...} (a table with string keys is an object)");
        json::Value out = json::Value::array();
        const json::Value* items = schema ? itemSchema(*schema) : nullptr;
        for (std::size_t i = 1; i <= length; ++i) {
            lua_rawgeti(L, table, static_cast<lua_Integer>(i));
            auto item = valueToJson(L, -1, items, path + "/" + std::to_string(i - 1));
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
        if (lua_type(L, -2) == LUA_TSTRING)
            key = lua_tostring(L, -2);
        else if (lua_isinteger(L, -2))
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
        const json::Value* member = schema ? memberSchema(*schema, key) : nullptr;
        auto value = valueToJson(L, -1, member, path + "/" + key);
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

void pushJson(lua_State* L, const json::Value& value) {
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
            lua_createtable(L, static_cast<int>(value.size()), 0);
            const int table = lua_gettop(L);
            int position = 1;
            for (const auto& item : value.asArray()) {
                pushJson(L, item);
                lua_rawseti(L, table, position++);
            }
            return;
        }
        case json::Type::Object: {
            lua_createtable(L, 0, static_cast<int>(value.size()));
            const int table = lua_gettop(L);
            for (const auto& member : value.asObject()) {
                pushJson(L, member.value);
                lua_setfield(L, table, member.key.c_str());
            }
            return;
        }
    }
}

Result<json::Value> toJson(lua_State* L, int index, const json::Value* schema,
                           const std::string& path) {
    return valueToJson(L, index, schema, path);
}

void pushError(lua_State* L, const Error& error) {
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
    lua_createtable(L, static_cast<int>(error.details.size()), 0);
    const int details = lua_gettop(L);
    int position = 1;
    for (const auto& detail : error.details) {
        pushError(L, detail);
        lua_rawseti(L, details, position++);
    }
    lua_setfield(L, table, "details");
}

Error errorFromLua(lua_State* L, int index) {
    if (lua_type(L, index) == LUA_TTABLE) {
        Error error;
        lua_getfield(L, index, "code");
        if (lua_type(L, -1) == LUA_TSTRING) error.code = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "message");
        if (lua_type(L, -1) == LUA_TSTRING) error.message = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "path");
        if (lua_type(L, -1) == LUA_TSTRING) error.path = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "hint");
        if (lua_type(L, -1) == LUA_TSTRING) error.hint = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, index, "details");
        if (lua_type(L, -1) == LUA_TTABLE) {
            const std::size_t count = lua_rawlen(L, -1);
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
                error.details.push_back(errorFromLua(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        if (!error.code.empty() || !error.message.empty()) {
            if (error.code.empty()) error.code = "script.error";
            return error;
        }
    }
    if (lua_type(L, index) == LUA_TSTRING) return Error("script.error", lua_tostring(L, index));
    return Error("script.error", "the script raised a non-string, non-table error value");
}

}  // namespace supple::lua
