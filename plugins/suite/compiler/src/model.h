// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What semantic analysis learns about a script's plugin: its identity, its
// parameter table, its hooks and the layout of its struct. Code generation
// and the runtime glue read it; the C ABI reports it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ast.h"
#include "supple/script_program.hpp"

namespace supple::scriptc {

struct BuiltinClass;

// One runtime object inside the plugin struct (directly, or within arrays
// and structs): constructed, copied and destroyed through its thunks.
struct ObjectSlot {
    int offset = 0;
    const BuiltinClass* cls = nullptr;
};

// A member's starting value, written when an instance is constructed.
struct InitialValue {
    int offset = 0;
    TypeKind kind = TypeKind::Double;  // Bool, Int, Float, Double, Enum
    double value = 0.0;
};

// A parameter member's own starting state.
struct ParamSlot {
    int offset = 0;
    int index = 0;
    double def = 0.0, min = 0.0;
    bool offAtMinimum = false;
};

struct PluginModel {
    StructDecl* decl = nullptr;
    PluginKind kind = PluginKind::None;
    InfoModel info;
    std::vector<ParamModel> params;
    std::vector<FieldDecl*> paramFields;  // params[i]'s member
    FunctionDecl* prepare = nullptr;
    FunctionDecl* update = nullptr;
    FunctionDecl* process = nullptr;
    int size = 0, align = 8;
    std::vector<ObjectSlot> objects;
    std::vector<InitialValue> initial;
    std::vector<ParamSlot> paramSlots;
};

// The result of semantic analysis.
struct Analysis {
    PluginModel plugin;
    std::vector<FunctionDecl*> functions;  // every function with a body, methods included
    std::vector<VarDecl*> globals;
};

// Checks the parsed unit and fills in the tree and the model. Errors go to
// `diags`; the analysis is complete only when there are none.
void analyse(Unit& unit, Analysis& out, Diagnostics& diags);

// The layout helpers code generation shares with sema: every runtime object
// inside a value of `type` placed at `base`.
void collectObjects(const Type* type, int base, std::vector<ObjectSlot>& out);

}  // namespace supple::scriptc
