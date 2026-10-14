// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Test-only syntax check using the same vendored Lua parser as the app.
#include <lauxlib.h>
#include <lua.h>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    lua_State* state = luaL_newstate();
    if (!state) return 2;
    const int result = luaL_loadfilex(state, argv[1], "t");
    if (result != LUA_OK) std::fprintf(stderr, "%s\n", lua_tostring(state, -1));
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
