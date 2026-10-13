// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Semantic analysis: declarations and their types, the layout of every
// struct, statements, the plugin (identity, parameters, hooks), and the
// checks that need the whole program: where each function runs, recursion,
// and C++'s unspecified evaluation order.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

#include "sema.h"
#include "supple/script.h"

namespace supple::scriptc {

namespace {

// Names a script may not declare: they are macros in the standard headers an
// exported plugin's translation unit also includes, or reserved to the
// implementation, so they would mean something else there.
bool reservedIdentifier(const std::string& n) {
    static const std::set<std::string> macros = {
        "NAN",         "INFINITY", "HUGE_VAL", "HUGE_VALF",    "M_PI",         "M_PI_2",
        "M_PI_4",      "M_E",      "M_SQRT2",  "M_LN2",        "M_LN10",       "M_LOG2E",
        "M_LOG10E",    "M_1_PI",   "M_2_PI",   "M_SQRT1_2",    "EOF",          "NULL",
        "errno",       "assert",   "stdin",    "stdout",       "stderr",       "INT_MAX",
        "INT_MIN",     "FLT_MAX",  "FLT_MIN",  "DBL_MAX",      "DBL_MIN",      "FLT_EPSILON",
        "DBL_EPSILON", "RAND_MAX", "CHAR_BIT", "EXIT_SUCCESS", "EXIT_FAILURE", "min",
        "max",         "small",    "near",     "far",          "interface",    "major",
        "minor",       "unix",     "linux"};
    if (macros.count(n)) return true;
    if (n.size() >= 2 && n[0] == '_' &&
        (std::isupper(static_cast<unsigned char>(n[1])) || n[1] == '_'))
        return true;
    return n.find("__") != std::string::npos;
}

// A member's name as a patch key: lowCut -> low_cut.
std::string snakeCase(const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (i > 0 && name[i - 1] != '_' &&
                (std::islower(static_cast<unsigned char>(name[i - 1])) ||
                 std::isdigit(static_cast<unsigned char>(name[i - 1])) ||
                 (i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1])))))
                out += '_';
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out;
}

bool validKey(const std::string& k) {
    // core-params: [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*
    bool start = true;
    for (char c : k) {
        if (start) {
            if (!(c >= 'a' && c <= 'z')) return false;
            start = false;
        } else if (c == '.') {
            start = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return !start && !k.empty();
}

const std::set<std::string>& roleVocabulary() {
    static const std::set<std::string> roles = {
        "level",  "gain",    "mix",       "brightness", "resonance", "time",      "rate",
        "depth",  "pitch",   "width",     "pan",        "drive",     "threshold", "ratio",
        "attack", "release", "frequency", "q",          "mode",      "enable",    "seed"};
    return roles;
}

constexpr int kMaxStructBytes = 64 << 20;  // a plugin's whole state; Buffers hold the big data
constexpr int kMaxFrameBytes = 64 << 10;   // one function's local arrays and structs

}  // namespace

Sema::Sema(Unit& unit, Analysis& out, Diagnostics& diags)
    : unit_(unit), out_(out), diags_(diags), b_(Builtins::get()) {}

void analyse(Unit& unit, Analysis& out, Diagnostics& diags) { Sema(unit, out, diags).run(); }

void Sema::run() {
    collect();
    if (diags_.hasErrors()) return;
    checkEnums();
    checkGlobals();
    checkStructs();
    for (FunctionDecl* f : out_.functions) signature(f);
    extractPlugin();
    if (diags_.hasErrors()) return;
    for (FunctionDecl* f : out_.functions) checkFunction(f);
    if (diags_.hasErrors()) return;
    contexts();
    if (diags_.hasErrors()) return;
    effects();
    sequencing();
    checkRules();
    if (diags_.hasErrors()) return;
    buildConstruction();
}

// --- declarations ---------------------------------------------------------------

void Sema::collect() {
    bool included = false;
    auto declare = [&](Decl* d, const std::string& what) {
        if (b_.reserved(d->name)) {
            error(d->loc, "script.name.builtin",
                  "'" + d->name + "' is the runtime's; name your " + what + " differently",
                  "choose another name: the runtime's names (the dsp-language guide) are taken");
            return false;
        }
        if (reservedIdentifier(d->name)) {
            error(d->loc, "script.name.reserved",
                  "'" + d->name +
                      "' is reserved (a macro or an implementation name in C++'s headers)",
                  "choose another name");
            return false;
        }
        if (structs_.count(d->name) || enums_.count(d->name) || functions_.count(d->name) ||
            globals_.count(d->name)) {
            error(d->loc, "script.name.duplicate", "'" + d->name + "' is declared twice",
                  "the subset has no overloading: give each its own name");
            return false;
        }
        return true;
    };
    std::function<void(StructDecl*)> addStruct = [&](StructDecl* s) {
        allStructs_.push_back(s);
        for (StructDecl* n : s->nested) {
            if (b_.reserved(n->name))
                error(
                    n->loc, "script.name.builtin",
                    "'" + n->name + "' is the runtime's; name your struct differently",
                    "choose another name: the runtime's names (the dsp-language guide) are taken");
            addStruct(n);
        }
        for (EnumDecl* e : s->enums) allEnums_.push_back(e);
        for (FunctionDecl* f : s->methods)
            if (f->body) out_.functions.push_back(f);
        // Member names: unique within the struct.
        std::set<std::string> names;
        for (FieldDecl* f : s->fields)
            if (!names.insert(f->name).second)
                error(f->loc, "script.name.duplicate",
                      s->name + " declares '" + f->name + "' twice",
                      "give each member its own name");
        for (FunctionDecl* f : s->methods)
            if (!names.insert(f->name).second)
                error(f->loc, "script.name.duplicate",
                      s->name + " declares '" + f->name + "' twice",
                      "the subset has no overloading: give each member function its own name");
        for (FieldDecl* f : s->fields)
            if (reservedIdentifier(f->name))
                error(f->loc, "script.name.reserved",
                      "'" + f->name + "' is reserved in C++'s headers", "choose another name");
    };
    for (Decl* d : unit_.decls) {
        switch (d->kind) {
            case DeclKind::Include: {
                auto* inc = static_cast<IncludeDecl*>(d);
                if (inc->header == "supple/script.h")
                    included = true;
                else
                    error(d->loc, "script.unsupported.include",
                          "#include <" + inc->header + "> is not available",
                          "a script includes only <supple/script.h>, which has everything it can "
                          "use");
                break;
            }
            case DeclKind::Using: {
                auto* u = static_cast<UsingDecl*>(d);
                std::string path;
                for (const auto& p : u->path) path += (path.empty() ? "" : "::") + p;
                if (path != "supple::script")
                    error(d->loc, "script.unsupported.using",
                          "using namespace " + path + " is not available",
                          path == "std" ? "the runtime has its own sin, min, clamp...; drop the std"
                                        : "delete the line: the runtime's names need no using");
                break;
            }
            case DeclKind::Struct: {
                auto* s = static_cast<StructDecl*>(d);
                if (declare(s, "struct")) structs_[s->name] = s;
                addStruct(s);
                break;
            }
            case DeclKind::Enum: {
                auto* e = static_cast<EnumDecl*>(d);
                if (declare(e, "enum")) enums_[e->name] = e;
                allEnums_.push_back(e);
                break;
            }
            case DeclKind::Function: {
                auto* f = static_cast<FunctionDecl*>(d);
                if (declare(f, "function")) functions_[f->name] = f;
                if (f->body) out_.functions.push_back(f);
                if (f->name == "main")
                    error(f->loc, "script.name.main",
                          "a script has no main(); the runtime calls the plugin's hooks",
                          "delete main(); the audio goes in process(Block& io)");
                break;
            }
            case DeclKind::Var: {
                auto* v = static_cast<VarDecl*>(d);
                if (declare(v, "constant")) globals_[v->name] = v;
                out_.globals.push_back(v);
                break;
            }
            default:
                break;
        }
    }
    if (!included)
        error(Loc{1, 1}, "script.include.missing",
              "a script starts with #include <supple/script.h>",
              "add #include <supple/script.h> as the first line");
}

StructDecl* Sema::findStruct(const std::string& name, StructDecl* scope) const {
    for (StructDecl* s = scope; s; s = s->outer) {
        if (s->name == name && s->outer == nullptr && !scope) return s;
        for (StructDecl* n : s->nested)
            if (n->name == name) return n;
    }
    auto it = structs_.find(name);
    return it == structs_.end() ? nullptr : it->second;
}

EnumDecl* Sema::findEnum(const std::string& name, StructDecl* scope) const {
    for (StructDecl* s = scope; s; s = s->outer)
        for (EnumDecl* e : s->enums)
            if (e->name == name) return e;
    auto it = enums_.find(name);
    return it == enums_.end() ? nullptr : it->second;
}

// --- types ------------------------------------------------------------------------

std::string Sema::typeName(const QualType& q) const {
    if (!q.type) return "?";
    return (q.isConst ? "const " : "") + q.type->name + (q.isRef ? "&" : "");
}

const Type* Sema::arrayType(const Type* element, int count) {
    const std::string name = "Array<" + element->name + ", " + std::to_string(count) + ">";
    auto it = arrays_.find(name);
    if (it != arrays_.end()) return it->second;
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::Array;
    t->name = name;
    t->element = element;
    t->count = count;
    t->align = element->align;
    const long long bytes =
        static_cast<long long>(element->size) * (count + 1);  // the spare element too
    t->size = bytes > kMaxStructBytes ? kMaxStructBytes + 1 : static_cast<int>(bytes);
    Type* raw = t.get();
    unit_.types.push_back(std::move(t));
    arrays_[name] = raw;
    return raw;
}

const Type* Sema::bufferType(const Type* element) {
    const std::string name = "Buffer<" + element->name + ">";
    auto it = arrays_.find(name);
    if (it != arrays_.end()) return it->second;
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::Buffer;
    t->name = name;
    t->element = element;
    t->size = static_cast<int>(sizeof(supple::script::Buffer<double>));
    t->align = 8;
    Type* raw = t.get();
    unit_.types.push_back(std::move(t));
    arrays_[name] = raw;
    return raw;
}

const Type* Sema::resolve(TypeSpec* spec, StructDecl* scope, bool allowVoid) {
    if (!spec) return nullptr;
    if (spec->resolved) return spec->resolved;
    const Type* t = nullptr;
    std::vector<std::string> path = spec->path;
    while (path.size() > 1 && (path[0] == "supple" || path[0] == "script"))
        path.erase(path.begin());
    const std::string& first = path.empty() ? std::string() : path[0];
    if (spec->unsupported) return nullptr;
    if (path.size() == 1) {
        if (first == "void")
            t = b_.voidType();
        else if (first == "bool")
            t = b_.boolType();
        else if (first == "int")
            t = b_.intType();
        else if (first == "float")
            t = b_.floatType();
        else if (first == "double")
            t = b_.doubleType();
        else if (first == "Array" || first == "Buffer") {
            const bool isArray = first == "Array";
            if (spec->typeArgs.empty() || (isArray && spec->valueArgs.size() != 1) ||
                (!isArray && spec->typeArgs.size() != 1) || spec->argIsType.empty() ||
                !spec->argIsType[0]) {
                error(spec->loc, "script.type.template",
                      isArray ? "write Array<type, size>, such as Array<float, 512>"
                              : "write Buffer<type>, such as Buffer<float>",
                      "an Array has a fixed size; a Buffer is sized in prepare()");
                return nullptr;
            }
            const Type* element = resolve(spec->typeArgs[0], scope);
            if (!element) return nullptr;
            if (spec->typeArgs[0]->isRef || spec->typeArgs[0]->isConst) {
                error(spec->loc, "script.type.template",
                      "an " + first + "'s elements are plain values, not const or references",
                      "drop the const or &: " + first +
                          (first == "Array" ? "<float, 8>" : "<float>"));
                return nullptr;
            }
            if (isArray) {
                if (element->kind == TypeKind::Object && element->builtin == b_.param()) {
                    error(spec->loc, "script.param.array", "parameters are declared one by one",
                          "Param band1Gain = decibels(...); Param band2Gain = ...;");
                    return nullptr;
                }
                if (b_.isFile(element)) {
                    error(spec->loc, "script.file.array", "files are declared one by one",
                          element->name + " kick = audioFile(\"Kick\"); " + element->name +
                              " snare = ...;");
                    return nullptr;
                }
                if (element->kind == TypeKind::Object && !element->builtin->declarable) {
                    error(spec->loc, "script.type.template",
                          "an Array cannot hold " + element->name,
                          "keep " + element->name +
                              " as a member of its own, or hold numbers in the Array");
                    return nullptr;
                }
                std::int64_t n = 0;
                if (!constantInt(spec->valueArgs[0], n) || n <= 0) {
                    error(spec->valueArgs[0]->loc, "script.type.array_size",
                          "an Array's size must be a positive constant int",
                          "Array<float, 512>, or a constexpr int");
                    return nullptr;
                }
                if (n > (1 << 24)) {
                    error(spec->valueArgs[0]->loc, "script.type.array_size",
                          "an Array of " + std::to_string(n) +
                              " elements is too large to live in the plugin",
                          "use a Buffer<T> and resize it in prepare()");
                    return nullptr;
                }
                t = arrayType(element, static_cast<int>(n));
                if (t->size > kMaxStructBytes)
                    error(spec->loc, "script.type.array_size", t->name + " is larger than 64 MiB",
                          "use a Buffer<T> and resize it in prepare()");
            } else {
                if (!(element->kind == TypeKind::Double || element->kind == TypeKind::Float ||
                      element->kind == TypeKind::Int)) {
                    error(spec->loc, "script.type.buffer", "a Buffer holds double, float or int",
                          "Buffer<float>, Buffer<double> or Buffer<int>; an Array<T, N> holds "
                          "other types");
                    return nullptr;
                }
                t = bufferType(element);
            }
        } else if (StructDecl* s = findStruct(first, scope)) {
            layout(s);
            t = s->type;
        } else if (EnumDecl* e = findEnum(first, scope)) {
            t = e->type;
        } else if (const BuiltinClass* c = b_.cls(first)) {
            t = c->type;
        } else if (const Type* named = b_.enumNamed(first)) {
            t = named;
        }
    } else if (path.size() >= 2) {
        // Outer::Inner, Oscillator::Shape.
        if (const BuiltinClass* c = b_.cls(first); c && path.size() == 2) {
            auto it = c->nestedTypes.find(path[1]);
            if (it != c->nestedTypes.end()) t = it->second;
        } else if (StructDecl* s = findStruct(first, scope)) {
            StructDecl* cur = s;
            for (size_t i = 1; i < path.size() && cur; ++i) {
                StructDecl* inner = nullptr;
                for (StructDecl* n : cur->nested)
                    if (n->name == path[i]) inner = n;
                if (inner) {
                    cur = inner;
                    if (i + 1 == path.size()) {
                        layout(cur);
                        t = cur->type;
                    }
                    continue;
                }
                for (EnumDecl* e : cur->enums)
                    if (e->name == path[i] && i + 1 == path.size()) t = e->type;
                break;
            }
        }
    }
    if (!t) {
        std::string full;
        for (const auto& p : spec->path) full += (full.empty() ? "" : "::") + p;
        std::string hint;
        if (full == "Smoother")
            hint =
                "parameters glide by themselves: declare it with .smoothed(ms) and read it with "
                ".at(i)";
        else if (full == "SmoothedValue")
            hint = "declare the parameter with .smoothed(ms) and read it with .at(i)";
        else if (full == "AudioBuffer" || full == "ProcessContext")
            hint = "process() takes Block& io: io[channel][frame]";
        error(spec->loc, "script.type.unknown", "unknown type '" + full + "'", hint);
        return nullptr;
    }
    if (t->isVoid() && !allowVoid) {
        error(spec->loc, "script.type.void", "void is only a function's result",
              "give it a value's type: double, float, int or bool");
        return nullptr;
    }
    spec->resolved = t;
    return t;
}

bool Sema::copyable(const Type* t) const {
    if (!t) return true;
    switch (t->kind) {
        case TypeKind::Object:
            return !t->builtin->resource;
        case TypeKind::Buffer:
            return false;
        case TypeKind::Array:
            return copyable(t->element);
        case TypeKind::Struct:
            for (FieldDecl* f : t->structDecl->fields)
                if (!f->isStatic && !copyable(f->qt.type)) return false;
            return true;
        default:
            return true;
    }
}

bool Sema::declarable(const Type* t) const {
    if (!t) return true;
    switch (t->kind) {
        case TypeKind::Object:
            return t->builtin->declarable && !t->builtin->resource && t->builtin != b_.param() &&
                   !b_.isFile(t);
        case TypeKind::Buffer:
            return false;
        case TypeKind::Array:
            return declarable(t->element);
        case TypeKind::Struct:
            for (FieldDecl* f : t->structDecl->fields)
                if (!f->isStatic && !declarable(f->qt.type)) return false;
            return true;
        default:
            return true;
    }
}

void collectObjects(const Type* type, int base, std::vector<ObjectSlot>& out) {
    if (!type) return;
    switch (type->kind) {
        case TypeKind::Object:
            if (type->builtin->construct) out.push_back({base, type->builtin});
            break;
        case TypeKind::Array:
            for (int i = 0; i <= type->count; ++i)
                collectObjects(type->element, base + i * type->element->size, out);
            break;
        case TypeKind::Struct:
            for (FieldDecl* f : type->structDecl->fields)
                if (!f->isStatic) collectObjects(f->qt.type, base + f->offset, out);
            break;
        default:
            break;
    }
}

void Sema::layout(StructDecl* s) {
    if (s->laidOut) return;
    if (s->layingOut) {
        error(s->loc, "script.type.recursive", "struct " + s->name + " contains itself",
              "a struct cannot hold a value of its own type");
        return;
    }
    s->layingOut = true;
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::Struct;
    t->name = s->name;
    t->structDecl = s;
    s->type = t.get();
    unit_.types.push_back(std::move(t));
    // Enums first: members may use them.
    for (EnumDecl* e : s->enums) {
        if (e->type) continue;
        auto et = std::make_unique<Type>();
        et->kind = TypeKind::Enum;
        et->name = s->name + "::" + e->name;
        et->size = 4;
        et->align = 4;
        et->enumDecl = e;
        e->type = et.get();
        unit_.types.push_back(std::move(et));
    }
    int offset = s->base != PluginKind::None ? b_.pluginBaseSize() : 0;
    int align = s->base != PluginKind::None ? 8 : 1;
    for (FieldDecl* f : s->fields) {
        const Type* ft = resolve(f->spec, s);
        if (!ft) continue;
        f->qt = QualType{ft, f->spec->isConst || f->spec->isConstexpr, false};
        if (f->spec->isRef) {
            error(f->loc, "script.type.reference_member", "a member cannot be a reference",
                  "hold the value itself, or an index into an Array");
            continue;
        }
        if (f->isStatic) {
            if (!f->spec->isConstexpr && !f->spec->isConst) {
                error(f->loc, "script.type.static_member",
                      "a static member would be shared by every instance of the plugin",
                      "make it a plain member, or a constant: static constexpr");
            }
            if (!(ft->isScalar() || (ft->kind == TypeKind::Object && ft->builtin == b_.info()))) {
                error(f->loc, "script.type.static_member",
                      "static constexpr members are numbers (or the plugin's Info)",
                      "make it a plain member, set in prepare()");
            }
            continue;
        }
        if (f->spec->isConst || f->spec->isConstexpr) {
            error(f->loc, "script.type.const_member", "a const member has no use in a script",
                  "make it static constexpr (a constant shared by all) or a plain member");
        }
        if (ft->kind == TypeKind::Object && ft->builtin == b_.info()) {
            error(f->loc, "script.plugin.info",
                  "Info is declared static constexpr: static constexpr Info info = {...};",
                  "static constexpr Info info = {.name = \"...\", .id = \"...\"};");
            continue;
        }
        if (ft->kind == TypeKind::Object && ft->builtin == b_.param() &&
            s->base == PluginKind::None) {
            error(f->loc, "script.param.location",
                  "parameters are members of the plugin struct itself",
                  "move '" + f->name + "' into the struct that derives from Effect");
            continue;
        }
        if (b_.isFile(ft) && s->base == PluginKind::None) {
            error(f->loc, "script.file.location", "files are members of the plugin struct itself",
                  "move '" + f->name + "' into the struct that derives from Effect");
            continue;
        }
        if (ft->kind == TypeKind::Object && !ft->builtin->declarable && ft->builtin != b_.param()) {
            error(f->loc, "script.type.member",
                  ft->name + " is the runtime's to make; it cannot be a member",
                  ft->builtin == b_.block()
                      ? "process() receives it: void process(Block& io)"
                      : "use the one the runtime passes in, such as process()'s Block& io");
            continue;
        }
        if (ft->kind == TypeKind::Struct) layout(ft->structDecl);
        const int a = std::max(1, ft->align);
        offset = (offset + a - 1) / a * a;
        f->offset = offset;
        offset += ft->size;
        align = std::max(align, a);
        if (offset > kMaxStructBytes) {
            error(f->loc, "script.type.size", s->name + " is larger than 64 MiB",
                  "keep large data in Buffers, sized in prepare()");
            offset = kMaxStructBytes;
        }
    }
    s->type->size = std::max(1, (offset + align - 1) / align * align);
    s->type->align = align;
    s->laidOut = true;
    s->layingOut = false;
}

void Sema::checkEnums() {
    for (EnumDecl* e : allEnums_) {
        if (!e->type) {
            auto et = std::make_unique<Type>();
            et->kind = TypeKind::Enum;
            et->name = e->outer ? e->outer->name + "::" + e->name : e->name;
            et->size = 4;
            et->align = 4;
            et->enumDecl = e;
            e->type = et.get();
            unit_.types.push_back(std::move(et));
        }
        int next = 0;
        e->values.clear();
        std::set<std::string> seen;
        for (auto& [name, value] : e->enumerators) {
            if (!seen.insert(name).second)
                error(e->loc, "script.name.duplicate", e->name + " lists '" + name + "' twice",
                      "give each enumerator its own name");
            if (value) {
                std::int64_t v = 0;
                if (!constantInt(value, v)) {
                    error(value->loc, "script.constant.required",
                          "an enumerator's value must be a constant int",
                          "use a literal or a constexpr int: enum Mode { Soft = 0, Hard = 2 };");
                } else {
                    next = static_cast<int>(v);
                }
            }
            e->values.push_back(next++);
        }
    }
}

void Sema::checkStructs() {
    for (StructDecl* s : allStructs_) layout(s);
    // Static constants and default member initializers.
    for (StructDecl* s : allStructs_) {
        self_ = s;
        for (FieldDecl* f : s->fields) {
            if (!f->qt.type) continue;
            const Type* t = f->qt.type;
            if (f->isStatic) {
                if (t->kind == TypeKind::Object) continue;  // Info: the plugin's
                if (!f->init) {
                    error(f->loc, "script.constant.required",
                          "a static constexpr member needs its value",
                          "give it one where it is declared: static constexpr double kMax = 1.0;");
                    continue;
                }
                f->init = convert(check(f->init), QualType{t, false, false}, f->init->loc,
                                  "'" + f->name + "'", f->init->kind == ExprKind::InitList);
                if (!f->init->constant)
                    error(f->init->loc, "script.constant.required",
                          "'" + f->name + "' must be a constant",
                          "use literals, constexpr constants and arithmetic on them");
                continue;
            }
            if (t->kind == TypeKind::Object && t->builtin == b_.param()) {
                if (!f->init)
                    error(f->loc, "script.param.declaration",
                          "a parameter is declared with its description",
                          "Param " + f->name + " = decibels(\"Gain\", -24, 24, 0);");
                continue;  // evaluated with the plugin
            }
            if (b_.isFile(t)) {
                if (!f->init)
                    error(f->loc, "script.file.declaration", "a file is declared with its name",
                          t->builtin == b_.convolver()
                              ? "Convolver " + f->name + " = impulseResponse(\"Room\");"
                              : "AudioFile " + f->name + " = audioFile(\"Sample\");");
                continue;  // evaluated with the plugin
            }
            if (!f->init) {
                if (t->isScalar())
                    error(f->loc, "script.member.initial",
                          "give '" + f->name +
                              "' a starting value (C++ leaves an unset member undefined)",
                          t->kind == TypeKind::Bool ? "bool " + f->name + " = false;"
                          : t->kind == TypeKind::Enum
                              ? t->name + " " + f->name + " = " + t->name + "::...;"
                              : t->name + " " + f->name + " = 0;");
                continue;
            }
            if (t->kind == TypeKind::Object || t->kind == TypeKind::Buffer) {
                if (f->init->kind != ExprKind::InitList ||
                    !static_cast<InitListExpr*>(f->init)->items.empty())
                    error(f->init->loc, "script.member.initial",
                          t->name + " starts empty; set it up in prepare()",
                          "declare it as " + t->name + " " + f->name + ";");
                continue;
            }
            const bool braced = f->init->kind == ExprKind::InitList;
            f->init = convert(check(f->init), QualType{t, false, false}, f->init->loc,
                              "the starting value of '" + f->name + "'", braced);
            if (t->isScalar() && !f->init->constant)
                error(f->init->loc, "script.member.constant",
                      "a member's starting value must be a constant",
                      "compute it in prepare() (sampleRate() is known there)");
        }
        self_ = nullptr;
    }
}

void Sema::checkGlobals() {
    for (VarDecl* v : out_.globals) {
        const Type* t = resolve(v->spec, nullptr);
        if (!t) continue;
        v->qt = QualType{t, true, false};
        if (!(v->spec->isConst || v->spec->isConstexpr)) {
            error(v->loc, "script.global.mutable",
                  "a variable outside the plugin struct would be shared by every instance of the "
                  "plugin",
                  "make it a member of the plugin struct, or a constant: constexpr double " +
                      v->name + " = ...;");
            continue;
        }
        if (!t->isScalar()) {
            error(v->loc, "script.global.type", "constants outside structs are numbers",
                  "put tables inside the plugin struct, filled in prepare()");
            continue;
        }
        if (!v->init) {
            error(v->loc, "script.constant.required",
                  "the constant '" + v->name + "' needs a value",
                  "give it a value where it is declared: constexpr int " + v->name + " = 4;");
            continue;
        }
        v->init = convert(check(v->init), QualType{t, false, false}, v->init->loc,
                          "'" + v->name + "'", v->init->kind == ExprKind::InitList);
        if (!v->init->constant)
            error(v->init->loc, "script.constant.required",
                  "'" + v->name + "' must be a constant expression",
                  "use literals, constexpr constants and arithmetic on them");
    }
}

// --- functions ------------------------------------------------------------------------

void Sema::signature(FunctionDecl* f) {
    StructDecl* scope = f->owner;
    const Type* r = resolve(f->result, scope, true);
    f->resultType = QualType{r, f->result->isConst, f->result->isRef};
    if (r && !r->isVoid() && !r->isScalar() && !f->result->isRef)
        error(f->result->loc, "script.function.result",
              f->name + "() returns " + r->name +
                  " by value; a function returns a number, a bool, an enum or nothing",
              "fill a struct passed by reference instead: void " + f->name + "(" + r->name +
                  "& out)");
    if (f->result->isRef)
        error(f->result->loc, "script.function.result_reference",
              "functions cannot return references in the subset",
              "return an index, or do the work where the object is");
    if (f->isConstexpr)
        error(f->loc, "script.unsupported.constexpr_function",
              "constexpr functions are not in the subset",
              "drop constexpr; constants are written with literals and arithmetic");
    for (VarDecl* p : f->params) {
        const Type* t = resolve(p->spec, scope);
        p->qt = QualType{t, p->spec->isConst, p->spec->isRef};
        if (!t) continue;
        if (!p->spec->isRef && !t->isScalar()) {
            if (t->kind == TypeKind::Object && t->builtin == b_.block())
                error(p->loc, "script.function.parameter",
                      "a Block is passed by reference: Block& io", "write Block& io");
            else
                error(p->loc, "script.function.parameter",
                      "'" + p->name + "' takes " + t->name + " by value, which copies it",
                      "take it by reference: " + std::string(copyable(t) ? "const " : "") +
                          t->name + "& " + p->name);
        }
        if (p->init) {
            p->init = convert(check(p->init), QualType{t, false, false}, p->init->loc,
                              "the default of '" + p->name + "'");
            if (!p->init->constant)
                error(p->init->loc, "script.constant.required",
                      "a default argument must be a constant",
                      "use a literal or a constexpr constant");
        }
    }
}

void Sema::declareLocal(VarDecl* v) {
    if (scopes_.empty()) scopes_.emplace_back();
    auto& scope = scopes_.back();
    if (scope.count(v->name)) {
        error(v->loc, "script.name.duplicate", "'" + v->name + "' is already declared here",
              "give it another name, or assign to the existing one without the type");
        return;
    }
    if (reservedIdentifier(v->name))
        error(v->loc, "script.name.reserved", "'" + v->name + "' is reserved in C++'s headers",
              "choose another name");
    scope[v->name] = v;
    v->id = static_cast<int>(fn_->locals.size());
    fn_->locals.push_back(v);
}

void Sema::checkLocal(VarDecl* v) {
    const VarDecl* outer = declaring_;
    declaring_ = v;
    checkLocalDecl(v);
    declaring_ = outer;
}

void Sema::checkLocalDecl(VarDecl* v) {
    TypeSpec* spec = v->spec;
    if (spec->isAuto) {
        if (!v->init) {
            error(v->loc, "script.local.auto", "auto needs a value to take its type from",
                  "give it a value (auto x = 0.0;) or write its type (double x = 0.0;)");
            return;
        }
        if (v->init->kind == ExprKind::InitList) {
            error(v->loc, "script.local.auto",
                  "auto with braces deduces std::initializer_list in C++",
                  "write the type: double " + v->name + " = ...;");
            return;
        }
        v->init = check(v->init);
        const Type* t = v->init->type();
        if (!t) return;
        if (t->isVoid() || t->kind == TypeKind::String || t->kind == TypeKind::Null) {
            error(v->loc, "script.local.auto", "'" + v->name + "' has no value to take",
                  "write its type instead of auto, and give it a number");
            return;
        }
        if (!spec->isRef && t->kind == TypeKind::Object && t->builtin == b_.param()) {
            error(v->loc, "script.local.param_copy", "auto would copy the parameter object",
                  "write double " + v->name + " = " + "...; to read its value");
            return;
        }
        if (!spec->isRef && b_.isFile(t)) {
            error(v->loc, "script.local.file", "auto would copy the " + t->name + " member",
                  "refer to it: auto& " + v->name + " = ...;");
            return;
        }
        v->qt = QualType{t, spec->isConst || spec->isConstexpr, spec->isRef};
        if (spec->isRef) {
            // auto& binds what it is given, keeping its constness.
            if (!v->init->lvalue) {
                if (!spec->isConst) {
                    error(v->loc, "script.local.reference",
                          "auto& needs a variable or a member to refer to",
                          "write auto " + v->name + " = ...; for a copy");
                    return;
                }
                if (!t->isScalar()) {
                    error(v->loc, "script.local.reference", "a reference to a temporary " + t->name,
                          "store it in a variable: auto " + v->name + " = ...;");
                    return;
                }
                v->qt.isRef = false;  // const auto& to a temporary number: a constant copy
            } else {
                v->qt.isConst = v->qt.isConst || v->init->qt.isConst;
                markAddressTaken(v->init);
            }
        } else {
            if (!copyable(t)) {
                error(v->loc, "script.type.copy", t->name + " holds memory and cannot be copied",
                      "refer to it: auto& " + v->name + " = ...;");
                return;
            }
            if (t->kind == TypeKind::Object && t->builtin == b_.block()) {
                error(v->loc, "script.local.block", "auto would copy the Block",
                      "use the Block itself, or auto& io2 = io;");
                return;
            }
            if (t->kind == TypeKind::Object && t->builtin == b_.param()) return;
            if (t->isScalar())
                v->init = convert(v->init, QualType{t, false, false}, v->loc, "'" + v->name + "'");
        }
        declareLocal(v);
        return;
    }
    const Type* t = resolve(spec, self_);
    if (!t) return;
    v->qt = QualType{t, spec->isConst || spec->isConstexpr, spec->isRef};
    if (spec->isStatic && !spec->isConstexpr)
        error(v->loc, "script.local.static", "a static local would be shared by every instance",
              "make it a member");
    if (!spec->isRef) {
        if (t->kind == TypeKind::Object && t->builtin == b_.param()) {
            error(v->loc, "script.param.location", "parameters are members of the plugin struct",
                  "declare it as a member of the plugin struct: Param gain = decibels(...);");
            return;
        }
        if (b_.isFile(t)) {
            error(v->loc, "script.file.location", "files are members of the plugin struct",
                  "refer to one (" + t->name + "& " + v->name +
                      " = ...;), or declare it as a member: " +
                      (t->builtin == b_.convolver() ? "Convolver room = impulseResponse(\"Room\");"
                                                    : "AudioFile sample = audioFile(\"Sample\");"));
            return;
        }
        if (t->kind == TypeKind::Object && t->builtin->resource) {
            error(v->loc, "script.local.resource",
                  t->name +
                      " allocates its memory, so it can only be a member of the plugin (sized in "
                      "prepare())",
                  "declare it as a member: " + t->name + " " + v->name + ";");
            return;
        }
        if (t->kind == TypeKind::Buffer) {
            error(v->loc, "script.local.resource", "a Buffer allocates, so it can only be a member",
                  "for scratch space in process() use a fixed Array<" + t->element->name + ", N>");
            return;
        }
        if (!declarable(t) && !(t->kind == TypeKind::Object && v->init)) {
            error(v->loc, "script.local.type", t->name + " cannot be declared in a function",
                  "make it a member of the plugin struct, set up in prepare()");
            return;
        }
        if (t->size > kMaxFrameBytes) {
            error(v->loc, "script.local.size",
                  "'" + v->name + "' is " + std::to_string(t->size) +
                      " bytes, more than a function's 64 KiB of locals",
                  "make it a member of the plugin");
            return;
        }
    }
    if (!v->init) {
        if (spec->isRef) {
            error(v->loc, "script.local.reference", "a reference must be bound when it is declared",
                  "bind it where it is declared: DelayLine& line = lines[c];");
            return;
        }
        if (t->isScalar()) {
            error(v->loc, "script.local.uninitialized",
                  "give '" + v->name + "' a value (C++ leaves an unset variable undefined)",
                  t->name + " " + v->name + " = " + (t->kind == TypeKind::Bool ? "false" : "0") +
                      ";");
            return;
        }
        if (t->kind == TypeKind::Object && !t->builtin->declarable) {
            error(v->loc, "script.local.uninitialized", t->name + " needs a value",
                  "refer to an existing one: " + t->name + "& x = ...;");
            return;
        }
        declareLocal(v);
        return;
    }
    const bool braced = v->init->kind == ExprKind::InitList;
    if (!braced) v->init = check(v->init);
    v->init = convert(v->init, v->qt, v->loc, "'" + v->name + "'", braced);
    if (spec->isConstexpr && !(v->init && v->init->constant))
        error(v->loc, "script.constant.required", "a constexpr variable needs a constant value",
              "use literals, constexpr constants and arithmetic on them, or drop constexpr");
    declareLocal(v);
}

void Sema::checkBlock(CompoundStmt* c, bool newScope) {
    if (newScope) scopes_.emplace_back();
    for (Stmt* s : c->body) checkStmt(s);
    if (newScope) scopes_.pop_back();
}

void Sema::checkStmt(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Compound:
            checkBlock(static_cast<CompoundStmt*>(s));
            return;
        case StmtKind::Empty:
            return;
        case StmtKind::Expr: {
            auto* e = static_cast<ExprStmt*>(s);
            if (e->expr->kind == ExprKind::Binary && static_cast<BinaryExpr*>(e->expr)->op == ",")
                error(e->expr->loc, "script.unsupported.comma",
                      "the comma operator is only allowed in a for's step", "write two statements");
            e->expr = check(e->expr);
            return;
        }
        case StmtKind::Decl:
            for (VarDecl* v : static_cast<DeclStmt*>(s)->vars) checkLocal(v);
            return;
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(s);
            if (i->isConstexpr)
                error(i->loc, "script.unsupported.if_constexpr",
                      "if constexpr is not in the subset", "write if");
            i->cond = condition(i->cond);
            scopes_.emplace_back();
            checkStmt(i->then);
            scopes_.pop_back();
            scopes_.emplace_back();
            checkStmt(i->otherwise);
            scopes_.pop_back();
            return;
        }
        case StmtKind::For: {
            auto* f = static_cast<ForStmt*>(s);
            scopes_.emplace_back();
            checkStmt(f->init);
            if (f->cond) f->cond = condition(f->cond);
            if (f->step) f->step = check(f->step);
            ++loopDepth_;
            scopes_.emplace_back();
            checkStmt(f->body);
            scopes_.pop_back();
            --loopDepth_;
            scopes_.pop_back();
            return;
        }
        case StmtKind::RangeFor: {
            auto* r = static_cast<RangeForStmt*>(s);
            r->range = check(r->range);
            const Type* rt = r->range->type();
            scopes_.emplace_back();
            if (rt && (rt->kind == TypeKind::Array || rt->kind == TypeKind::Buffer)) {
                if (!r->range->lvalue)
                    error(r->range->loc, "script.loop.range",
                          "loop over a variable or a member, not a temporary",
                          "store it in a variable first, then loop over that");
                const Type* et = rt->element;
                VarDecl* v = r->var;
                TypeSpec* spec = v->spec;
                const Type* vt = spec->isAuto ? et : resolve(spec, self_);
                if (vt) {
                    v->qt = QualType{vt, spec->isConst || (r->range->qt.isConst && spec->isRef),
                                     spec->isRef};
                    if (spec->isRef) {
                        if (vt != et)
                            error(v->loc, "script.loop.reference",
                                  "the reference's type must be the elements' type, " + et->name,
                                  "or write auto&");
                        if (!spec->isConst && r->range->qt.isConst)
                            error(v->loc, "script.loop.const", "the elements are read-only here",
                                  "write const auto&");
                        markAddressTaken(r->range);
                    } else {
                        if (!copyable(et))
                            error(v->loc, "script.type.copy", et->name + " cannot be copied",
                                  "write auto& instead");
                        if (et->isScalar() && vt != et) {
                            // A converting copy: double x : floats.
                            ConversionRank rk = standardRank(et, vt);
                            if (rk.rank == ConversionRank::NotViable ||
                                (et->isFloating() && vt->kind == TypeKind::Int))
                                error(v->loc, "script.loop.type",
                                      "the elements are " + et->name + ", not " + vt->name,
                                      "declare the loop variable as " + et->name);
                        } else if (vt != et) {
                            error(v->loc, "script.loop.type",
                                  "the elements are " + et->name + ", not " + vt->name,
                                  "declare the loop variable as " + et->name);
                        }
                    }
                    declareLocal(v);
                }
            } else if (rt) {
                error(r->range->loc, "script.loop.range",
                      "a range-based for walks an Array or a Buffer, not " + rt->name,
                      rt->kind == TypeKind::Object && rt->builtin == b_.block()
                          ? "loop over channels and frames: for (int c = 0; c < io.channels; ++c)"
                          : "loop with an index: for (int i = 0; i < n; ++i)");
            }
            ++loopDepth_;
            scopes_.emplace_back();
            checkStmt(r->body);
            scopes_.pop_back();
            --loopDepth_;
            scopes_.pop_back();
            return;
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            w->cond = condition(w->cond);
            ++loopDepth_;
            scopes_.emplace_back();
            checkStmt(w->body);
            scopes_.pop_back();
            --loopDepth_;
            return;
        }
        case StmtKind::DoWhile: {
            auto* d = static_cast<DoWhileStmt*>(s);
            ++loopDepth_;
            scopes_.emplace_back();
            checkStmt(d->body);
            scopes_.pop_back();
            --loopDepth_;
            d->cond = condition(d->cond);
            return;
        }
        case StmtKind::Switch: {
            auto* sw = static_cast<SwitchStmt*>(s);
            sw->cond = check(sw->cond);
            const Type* t = sw->cond->type();
            if (t) {
                if (t->kind == TypeKind::Object && t->builtin == b_.param())
                    error(sw->cond->loc, "script.switch.param",
                          "switch on a parameter needs its index",
                          "switch (mode.index()) { case 0: ... }");
                else if (!(t->isIntegral() || t->kind == TypeKind::Enum))
                    error(sw->cond->loc, "script.switch.type",
                          "switch needs an int or an enum, not " + t->name,
                          t->isFloating() ? "compare with if/else instead"
                                          : "switch on an int or an enum value");
                else if (t->kind == TypeKind::Bool)
                    sw->cond = convert(sw->cond, QualType{b_.intType(), false, false},
                                       sw->cond->loc, "the switch");
            }
            if (!sw->body) return;
            ++switchDepth_;
            scopes_.emplace_back();
            std::set<std::int64_t> seen;
            bool hasDefault = false;
            bool declaredSinceCase = false;
            Loc declLoc;
            for (Stmt* st : sw->body->body) {
                if (st->kind == StmtKind::Case) {
                    auto* c = static_cast<CaseStmt*>(st);
                    if (declaredSinceCase)
                        error(c->loc, "script.switch.jump",
                              "this case jumps over the declaration on line " +
                                  std::to_string(declLoc.line) + " (C++ forbids it)",
                              "wrap that case's statements in braces: case 1: { ... break; }");
                    if (!c->value) {
                        if (hasDefault)
                            error(c->loc, "script.switch.default", "two default labels",
                                  "keep one default: label");
                        hasDefault = true;
                        continue;
                    }
                    c->value = check(c->value);
                    const Type* ct = c->value->type();
                    if (!c->value->constant || !ct ||
                        !(ct->isIntegral() || ct->kind == TypeKind::Enum)) {
                        error(c->value->loc, "script.switch.case",
                              "a case label must be a constant int or enum value",
                              "use a literal, a constexpr int or an enum value: case 2:");
                        continue;
                    }
                    if (t && t->kind == TypeKind::Enum && ct != t)
                        error(c->value->loc, "script.switch.case",
                              "a case label of " + t->name + " must be one of its values",
                              "name one of its values: case " + t->name + "::...:");
                    if (t && t->kind != TypeKind::Enum && ct->kind == TypeKind::Enum)
                        error(c->value->loc, "script.switch.case",
                              "an enum value as the case of an int switch",
                              "switch on the enum value itself, so its cases are its values");
                    c->constValue = c->value->constInt;
                    if (!seen.insert(c->constValue).second)
                        error(c->value->loc, "script.switch.duplicate",
                              "two cases with the value " + std::to_string(c->constValue),
                              "merge the two cases, or give one another value");
                    continue;
                }
                if (st->kind == StmtKind::Decl && !declaredSinceCase) {
                    declaredSinceCase = true;
                    declLoc = st->loc;
                }
                checkStmt(st);
            }
            scopes_.pop_back();
            --switchDepth_;
            return;
        }
        case StmtKind::Case:
            error(s->loc, "script.switch.case_outside",
                  "a case label belongs directly inside a switch's braces",
                  "put it inside a switch: switch (x) { case 1: ... break; }");
            return;
        case StmtKind::Break:
            if (loopDepth_ == 0 && switchDepth_ == 0)
                error(s->loc, "script.statement.break", "break outside a loop or a switch",
                      "remove it, or use return to leave the function");
            return;
        case StmtKind::Continue:
            if (loopDepth_ == 0)
                error(s->loc, "script.statement.continue", "continue outside a loop",
                      "remove it: continue only skips to a loop's next turn");
            return;
        case StmtKind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            const QualType rt = fn_->resultType;
            if (!rt.type) return;
            if (rt.type->isVoid()) {
                if (r->value) {
                    r->value = check(r->value);
                    if (!r->value->type() || !r->value->type()->isVoid())
                        error(r->loc, "script.return.value", fn_->name + "() returns nothing",
                              "write return;");
                }
                return;
            }
            if (!r->value) {
                error(r->loc, "script.return.missing",
                      fn_->name + "() must return " + rt.type->name,
                      "end with return and a value: return x;");
                return;
            }
            const bool braced = r->value->kind == ExprKind::InitList;
            if (!braced) r->value = check(r->value);
            r->value = convert(r->value, QualType{rt.type, false, false}, r->loc,
                               "the result of " + fn_->name + "()", braced);
            return;
        }
    }
}

// Whether every path through s ends in a return.
bool Sema::returns(Stmt* s) const {
    if (!s) return false;
    switch (s->kind) {
        case StmtKind::Return:
            return true;
        case StmtKind::Compound:
            for (Stmt* x : static_cast<CompoundStmt*>(s)->body)
                if (returns(x)) return true;
            return false;
        case StmtKind::If: {
            auto* i = static_cast<IfStmt*>(s);
            return i->otherwise && returns(i->then) && returns(i->otherwise);
        }
        case StmtKind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            // while (true) without a break never falls out.
            if (!(w->cond && w->cond->constant && w->cond->constInt)) return false;
            std::function<bool(Stmt*)> breaks = [&](Stmt* x) -> bool {
                if (!x) return false;
                if (x->kind == StmtKind::Break) return true;
                if (x->kind == StmtKind::Compound) {
                    for (Stmt* y : static_cast<CompoundStmt*>(x)->body)
                        if (breaks(y)) return true;
                }
                if (x->kind == StmtKind::If)
                    return breaks(static_cast<IfStmt*>(x)->then) ||
                           breaks(static_cast<IfStmt*>(x)->otherwise);
                return false;
            };
            return !breaks(w->body);
        }
        case StmtKind::Switch: {
            auto* sw = static_cast<SwitchStmt*>(s);
            if (!sw->body) return false;
            bool hasDefault = false;
            for (Stmt* x : sw->body->body)
                if (x->kind == StmtKind::Case && !static_cast<CaseStmt*>(x)->value)
                    hasDefault = true;
            if (!hasDefault) return false;
            // Every case's statements must end in a return before a break or the end.
            bool sawReturn = false;
            for (Stmt* x : sw->body->body) {
                if (x->kind == StmtKind::Case) continue;
                if (x->kind == StmtKind::Break) return false;
                if (returns(x)) sawReturn = true;
            }
            return sawReturn && returns(sw->body->body.back());
        }
        default:
            return false;
    }
}

void Sema::checkFunction(FunctionDecl* f) {
    fn_ = f;
    self_ = f->owner;
    scopes_.clear();
    scopes_.emplace_back();
    for (VarDecl* p : f->params) {
        if (p->name.empty()) continue;
        declareLocal(p);
    }
    if (f->body) checkBlock(f->body, true);
    if (f->resultType.type && !f->resultType.type->isVoid() && !returns(f->body))
        error(
            f->loc, "script.return.missing_path",
            f->name + "() can reach its end without returning a value (C++ leaves that undefined)",
            "add a return at the end of the function");
    scopes_.clear();
    fn_ = nullptr;
    self_ = nullptr;
}

// --- the plugin ------------------------------------------------------------------------

void Sema::extractPlugin() {
    PluginModel& p = out_.plugin;
    std::vector<StructDecl*> plugins;
    for (StructDecl* s : allStructs_)
        if (s->base != PluginKind::None) plugins.push_back(s);
    if (plugins.empty()) {
        error(Loc{1, 1}, "script.plugin.missing", "the script declares no plugin",
              "declare one struct that derives from Effect (audio in and out): "
              "struct MyPlugin : Effect { ... };");
        return;
    }
    if (plugins.size() > 1) {
        error(plugins[1]->loc, "script.plugin.two",
              "a script declares one plugin; " + plugins[0]->name + " is already one",
              "make the others plain structs and hold them as members");
        return;
    }
    StructDecl* s = plugins[0];
    if (s->outer) {
        error(s->loc, "script.plugin.nested", "the plugin struct is declared at the top level",
              "move the struct that derives from Effect to the top level of the file");
        return;
    }
    p.decl = s;
    p.kind = s->base;
    // Notes out are not available yet: no note effects.
    if (p.kind == PluginKind::NoteEffect)
        error(s->baseLoc, "script.plugin.kind",
              "note effects are not available to scripts yet (scripts send no notes)",
              "derive from Effect (audio in, audio out) or Instrument (notes in, audio out)");
    // Info.
    FieldDecl* infoField = nullptr;
    for (FieldDecl* f : s->fields)
        if (f->isStatic && f->qt.type && f->qt.type->kind == TypeKind::Object &&
            f->qt.type->builtin == b_.info())
            infoField = f;
    if (!infoField) {
        error(s->loc, "script.plugin.info", s->name + " needs its identity",
              "add  static constexpr Info info = {.name = \"" + s->name +
                  "\", .id = \"com.example." + snakeCase(s->name) + "\"};");
    } else {
        evaluateInfo(infoField);
    }
    // Parameters.
    std::set<std::string> keys;
    std::map<std::uint32_t, std::string> ids;
    for (FieldDecl* f : s->fields) {
        if (!f->qt.type || f->isStatic || f->qt.type->kind != TypeKind::Object ||
            f->qt.type->builtin != b_.param())
            continue;
        if (!f->init) continue;
        evaluateParam(f);
    }
    for (size_t i = 0; i < p.params.size(); ++i) {
        const ParamModel& pm = p.params[i];
        const Loc at = p.paramFields[i]->loc;
        if (!keys.insert(pm.key).second)
            error(at, "script.param.key", "two parameters have the key '" + pm.key + "'",
                  "rename one, or give it a key of its own: .withKey(\"other_key\")");
        auto [it, fresh] = ids.emplace(pm.id, pm.key);
        if (!fresh)
            error(at, "script.param.id",
                  "'" + pm.key + "' and '" + it->second + "' have the same id " +
                      std::to_string(pm.id),
                  "give one an explicit id: .withId(n)");
    }
    // Files.
    for (FieldDecl* f : s->fields)
        if (!f->isStatic && f->init && b_.isFile(f->qt.type)) evaluateFile(f);
    std::set<std::string> fileKeys;
    for (size_t i = 0; i < p.files.size(); ++i)
        if (!fileKeys.insert(p.files[i].key).second)
            error(p.fileFields[i]->loc, "script.file.key",
                  "two files have the key '" + p.files[i].key + "'",
                  "rename one, or give it a key of its own: .withKey(\"other_key\")");
    // Hooks.
    for (FunctionDecl* f : s->methods) {
        const std::string& n = f->name;
        auto expectSignature = [&](bool ok, const std::string& want) {
            if (!ok)
                error(f->loc, "script.plugin.hook",
                      n + "() is a hook the runtime calls; declare it as " + want, want);
            return ok;
        };
        const bool isVoid = f->resultType.type && f->resultType.type->isVoid();
        if (n == "prepare") {
            if (expectSignature(isVoid && f->params.empty() && !f->isConst, "void prepare()"))
                p.prepare = f;
        } else if (n == "update") {
            if (expectSignature(isVoid && f->params.empty() && !f->isConst, "void update()"))
                p.update = f;
        } else if (n == "process") {
            const bool ok = isVoid && f->params.size() == 1 && f->params[0]->qt.type &&
                            f->params[0]->qt.type->kind == TypeKind::Object &&
                            f->params[0]->qt.type->builtin == b_.block() &&
                            f->params[0]->qt.isRef && !f->params[0]->qt.isConst && !f->isConst;
            if (expectSignature(ok, "void process(Block& io)")) p.process = f;
        } else if (n == "startVoice" || n == "releaseVoice" || n == "stopVoice") {
            if (p.kind != PluginKind::Instrument) {
                error(f->loc, "script.plugin.hook",
                      n + "() is an instrument's hook, and " + s->name + " gets no notes",
                      "derive from Instrument to play notes, or give the function another name");
                continue;
            }
            auto plain = [&](size_t i, const Type* t) {
                return f->params.size() > i && f->params[i]->qt.type == t &&
                       !f->params[i]->qt.isRef;
            };
            const bool takesVoice = isVoid && !f->isConst && plain(0, b_.intType());
            if (n == "startVoice") {
                const bool one = takesVoice && f->params.size() == 1;
                const bool two = takesVoice && f->params.size() == 2 && plain(1, b_.boolType());
                if (expectSignature(one || two, "void startVoice(int v)"))
                    (two ? p.startVoiceRetrigger : p.startVoice) = f;
            } else if (expectSignature(takesVoice && f->params.size() == 1,
                                       "void " + n + "(int v)")) {
                (n == "releaseVoice" ? p.releaseVoice : p.stopVoice) = f;
            }
        } else if (n == "reset") {
            error(
                f->loc, "script.plugin.reset",
                "reset is the runtime's: it returns every member to what prepare() left, by itself",
                "remove reset(); clear nothing by hand");
        } else if (n == "processBlock" || n == "processSample" || n == "processAudio" ||
                   n == "render" || n == "run") {
            error(f->loc, "script.plugin.hook_name",
                  "the runtime calls process(Block& io), not " + n + "()",
                  "rename it: void process(Block& io)");
        } else if (n == "prepareToPlay" || n == "init" || n == "setup" || n == "initialize" ||
                   n == "setSampleRate") {
            error(f->loc, "script.plugin.hook_name", "the runtime calls prepare(), not " + n + "()",
                  "rename it: void prepare()  (sampleRate() and maxBlockSize() are known there)");
        } else if (n == "parameterChanged" || n == "onParamChange" || n == "updateParameters") {
            error(f->loc, "script.plugin.hook_name",
                  "the runtime calls update() after parameters change, not " + n + "()",
                  "rename it: void update()");
        }
    }
    if (p.kind == PluginKind::Instrument) {
        if (!p.startVoice && !p.startVoiceRetrigger)
            error(s->loc, "script.plugin.voices", s->name + " needs  void startVoice(int v)",
                  "the runtime calls it when voice v starts a note: start its envelope there, "
                  "reading voiceKey(v) and voiceVelocity(v)");
        if (!p.releaseVoice)
            error(s->loc, "script.plugin.voices", s->name + " needs  void releaseVoice(int v)",
                  "the runtime calls it when voice v's key is up: start its release there, and "
                  "call voiceFinished(v) in process() once it has died away");
    }
    if (!p.process && p.kind != PluginKind::NoteEffect)
        error(s->loc, "script.plugin.process", s->name + " needs  void process(Block& io)",
              p.kind == PluginKind::Effect
                  ? "io[channel][frame] holds the input; write the output in place"
                  : "write the output into io[channel][frame] (it arrives silent)");
}

void Sema::evaluateInfo(FieldDecl* f) {
    InfoModel& info = out_.plugin.info;
    if (!f->init || f->init->kind != ExprKind::InitList) {
        error(f->loc, "script.plugin.info", "Info is written with designated members",
              "static constexpr Info info = {.name = \"Tilt\", .id = \"com.example.tilt\"};");
        return;
    }
    static const std::vector<std::string> order = {"name",        "id",     "vendor",   "version",
                                                   "description", "url",    "channels", "sidechain",
                                                   "outputs",     "voices", "latency",  "tail"};
    auto* list = static_cast<InitListExpr*>(f->init);
    size_t last = 0;
    for (auto& item : list->items) {
        if (item.designator.empty()) {
            error(item.loc, "script.plugin.info", "name each of Info's members: .name = \"...\"",
                  "{.name = \"Tilt\", .id = \"com.example.tilt\"}");
            continue;
        }
        auto pos = std::find(order.begin(), order.end(), item.designator);
        if (pos == order.end()) {
            std::string all;
            for (const auto& o : order) all += (all.empty() ? "" : ", ") + o;
            error(item.loc, "script.plugin.info", "Info has no member '" + item.designator + "'",
                  "its members: " + all);
            continue;
        }
        const size_t index = static_cast<size_t>(pos - order.begin());
        if (index < last) {
            error(item.loc, "script.type.designator_order",
                  "designated initializers must follow Info's order: name, id, vendor, version, "
                  "description, url, "
                  "channels, sidechain, outputs, voices, latency, tail",
                  "move ." + item.designator + " earlier");
        }
        last = index;
        auto v = evaluate(item.value, out_.plugin.decl);
        if (!v) continue;
        const std::string& d = item.designator;
        auto text = [&](std::string& target) {
            if (v->kind != Constant::Kind::String) {
                error(item.loc, "script.plugin.info", "." + d + " is text in quotes",
                      "." + d + " = \"...\"");
                return;
            }
            target = v->stringValue;
        };
        auto integer = [&](int& target) {
            if (v->kind != Constant::Kind::Int) {
                error(item.loc, "script.plugin.info", "." + d + " is a whole number",
                      "." + d + " = 0");
                return;
            }
            target = static_cast<int>(v->intValue);
        };
        if (d == "name")
            text(info.name);
        else if (d == "id")
            text(info.id);
        else if (d == "vendor")
            text(info.vendor);
        else if (d == "version")
            text(info.version);
        else if (d == "description")
            text(info.description);
        else if (d == "url")
            text(info.url);
        else if (d == "sidechain") {
            if (v->kind != Constant::Kind::Bool)
                error(item.loc, "script.plugin.info", ".sidechain is true or false",
                      ".sidechain = true");
            info.sidechain = v->boolValue;
        } else if (d == "voices")
            integer(info.voices);
        else if (d == "latency")
            integer(info.latency);
        else if (d == "tail")
            integer(info.tail);
        else if (d == "channels") {
            if (v->kind != Constant::Kind::List) {
                error(item.loc, "script.plugin.info", ".channels is a list: {2, 1}",
                      ".channels = {2, 1}: stereo, else mono");
                continue;
            }
            for (const Constant& c : v->list) {
                if (c.kind != Constant::Kind::Int || c.intValue < 1 || c.intValue > 16) {
                    error(item.loc, "script.plugin.info", ".channels lists bus widths from 1 to 16",
                          ".channels = {2, 1}");
                    break;
                }
                info.channels.push_back(static_cast<int>(c.intValue));
            }
            if (info.channels.size() > 4)
                error(item.loc, "script.plugin.info", "at most four channel layouts",
                      "keep the four layouts the plugin needs most");
        } else if (d == "outputs") {
            if (v->kind != Constant::Kind::List) {
                error(item.loc, "script.plugin.info",
                      ".outputs is a list of names: {\"Kick\", \"Snare\"}",
                      ".outputs = {\"Kick\", \"Snare\"}");
                continue;
            }
            for (const Constant& c : v->list) {
                if (c.kind != Constant::Kind::String || c.stringValue.empty()) {
                    error(item.loc, "script.plugin.info", ".outputs lists names in quotes",
                          ".outputs = {\"Kick\", \"Snare\"}");
                    break;
                }
                info.outputs.push_back(c.stringValue);
            }
            if (info.outputs.size() > 8)
                error(item.loc, "script.plugin.info", "at most eight extra outputs",
                      "keep eight; mix the rest into the main output");
        }
    }
    if (info.name.empty())
        error(f->loc, "script.plugin.name", "Info needs a .name", ".name = \"My Effect\"");
    if (info.name.size() > 63)
        error(f->loc, "script.plugin.name", "the plugin's name is longer than 63 bytes",
              "shorten it; .description holds the rest");
    // Reverse-DNS, as plugin formats and the framework take it: labels of
    // letters, digits and '-', two or more, joined by '.'.
    std::string suggested = snakeCase(info.name.empty() ? std::string("plugin") : info.name);
    for (char& c : suggested)
        if (c == '_') c = '-';
    if (info.id.empty()) {
        error(f->loc, "script.plugin.id",
              "Info needs an .id: a reverse-DNS name that identifies the plugin forever",
              ".id = \"com.yourname." + suggested + "\"");
    } else {
        bool ok = info.id.size() <= 255 && info.id.find('.') != std::string::npos &&
                  info.id.front() != '.' && info.id.back() != '.' &&
                  info.id.find("..") == std::string::npos;
        for (char c : info.id)
            ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-');
        if (!ok)
            error(f->loc, "script.plugin.id",
                  "Info's .id is a reverse-DNS name: words of letters, digits and '-', joined by "
                  "'.'",
                  ".id = \"com.yourname." + suggested + "\"");
    }
    {
        // MAJOR.MINOR.PATCH, which every plugin format can show.
        int parts = 0;
        bool digits = false, ok = true;
        for (char c : info.version) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                digits = true;
            } else if (c == '.' && digits) {
                ++parts;
                digits = false;
            } else {
                ok = false;
            }
        }
        if (!ok || !digits || parts != 2)
            error(f->loc, "script.plugin.info", "'" + info.version + "' is not a version",
                  ".version = \"1.2.0\" (major, minor, patch)");
    }
    if (info.voices < 0 || info.voices > 64)
        error(f->loc, "script.plugin.info", ".voices is from 1 to 64 (0: the default, 8)",
              ".voices = 8");
    if (info.latency < 0)
        error(f->loc, "script.plugin.info", ".latency cannot be negative",
              ".latency = 0, or the delay the plugin adds, in samples");
    if (info.tail < -1)
        error(f->loc, "script.plugin.info", ".tail is samples, or -1 for forever",
              ".tail = 0 (it stops with its input), the tail in samples, or -1 (it rings forever)");
}

void Sema::evaluateFile(FieldDecl* f) {
    const bool convolver = f->qt.type->builtin == b_.convolver();
    const std::string want = convolver ? "Convolver " + f->name + " = impulseResponse(\"Room\");"
                                       : "AudioFile " + f->name + " = audioFile(\"Sample\");";
    auto value = evaluate(f->init, out_.plugin.decl);
    if (!value) return;
    if (value->kind != Constant::Kind::File) {
        error(f->init->loc, "script.file.declaration", "a file is declared with its name", want);
        return;
    }
    FileModel m = value->file;
    if ((m.kind == FileModel::Kind::Impulse) != convolver) {
        error(f->init->loc, "script.file.kind",
              convolver ? "a Convolver's file is an impulse response"
                        : "an AudioFile is declared with audioFile(); impulse responses are a "
                          "Convolver's",
              want);
        return;
    }
    m.member = f->name;
    if (m.key.empty()) m.key = snakeCase(f->name);
    if (!validKey(m.key))
        error(f->loc, "script.file.key", "'" + m.key + "' is not a usable patch key",
              "keys are lowercase letters, digits and _: rename the member, or give "
              ".withKey(\"sample\")");
    m.id = script::detail::keyId(m.key);
    m.offset = f->offset;
    if (m.name.empty() || m.name.size() > 63)
        error(f->init->loc, "script.file.declaration", "a file's name is between 1 and 63 bytes",
              want);
    out_.plugin.files.push_back(std::move(m));
    out_.plugin.fileFields.push_back(f);
}

void Sema::evaluateParam(FieldDecl* f) {
    auto value = evaluate(f->init, out_.plugin.decl);
    if (!value) return;
    if (value->kind != Constant::Kind::Spec) {
        error(
            f->init->loc, "script.param.declaration", "a parameter is declared with a factory",
            "Param " + f->name +
                " = decibels(\"Gain\", -24, 24, 0);  (or frequency, milliseconds, percent, ratio, "
                "semitones, cents, integer, choice, toggle)");
        return;
    }
    ParamModel p = value->spec;
    p.member = f->name;
    if (p.key.empty()) p.key = snakeCase(f->name);
    if (!validKey(p.key))
        error(f->loc, "script.param.key", "'" + p.key + "' is not a usable patch key",
              "keys are lowercase letters, digits and _: rename the member, or give "
              ".withKey(\"low_cut\")");
    if (p.id == 0) p.id = script::detail::keyId(p.key);
    p.offset = f->offset;
    auto bad = [&](const std::string& what, const std::string& hint = {}) {
        error(f->init->loc, "script.param.range", p.name + ": " + what, hint);
    };
    if (p.name.empty()) bad("a parameter needs a name");
    if (p.name.size() > 63) bad("the name is longer than 63 bytes");
    if (!std::isfinite(p.min) || !std::isfinite(p.max) || !std::isfinite(p.def))
        bad("its range must be finite");
    if (!(p.max > p.min)) bad("the maximum must be above the minimum");
    if (p.def < p.min || p.def > p.max)
        bad("the default " + std::to_string(p.def) + " is outside " + std::to_string(p.min) + ".." +
            std::to_string(p.max));
    if (p.curve == ParamModel::Curve::Log && p.min <= 0)
        bad("a logarithmic range must start above 0");
    if (p.curve == ParamModel::Curve::Pow && !(p.curveK > 0))
        bad("a power curve's exponent must be above 0");
    if (p.curve == ParamModel::Curve::Log && p.max / p.min > 1e15)
        bad("a logarithmic range spans at most 15 decades");
    if (p.kind == ParamModel::Kind::Choice) {
        if (p.choices.size() < 2) bad("a choice needs at least two options");
        std::set<std::string> seen;
        for (const auto& c : p.choices) {
            if (c.empty() || c.size() > 31) bad("each option is 1 to 31 bytes");
            if (!seen.insert(c).second) bad("the option '" + c + "' appears twice");
        }
    }
    if (p.kind != ParamModel::Kind::Continuous && p.def != std::floor(p.def))
        bad("its default must be a whole number");
    if (p.kind == ParamModel::Kind::Integer &&
        (p.min != std::floor(p.min) || p.max != std::floor(p.max)))
        bad("an integer parameter's range is whole numbers");
    if (p.smoothingMs < 0) bad("a smoothing time cannot be negative");
    if (p.smoothingMs > 0 && p.kind != ParamModel::Kind::Continuous)
        bad("only continuous parameters glide", "remove .smoothed()");
    if (p.smoothingMs > 0 && p.output) bad("a meter does not glide");
    if (p.offAtMinimum &&
        !(p.kind == ParamModel::Kind::Continuous || p.kind == ParamModel::Kind::Integer))
        bad(".offAtMin() is for continuous and integer parameters");
    if (p.output && p.modulatable) bad("a meter cannot be modulated");
    for (const auto& r : p.roles)
        if (!roleVocabulary().count(r)) {
            std::string all;
            for (const auto& x : roleVocabulary()) all += (all.empty() ? "" : ", ") + x;
            bad("unknown role '" + r + "'", "roles: " + all);
        }
    p.choices.shrink_to_fit();
    f->paramIndex = static_cast<int>(out_.plugin.params.size());
    out_.plugin.params.push_back(std::move(p));
    out_.plugin.paramFields.push_back(f);
}

// Starting values for a value of `type` at `base`, from its initializer (or
// the type's own defaults).
void Sema::initialValues(const Type* type, int base, Expr* init, StructDecl* scope) {
    PluginModel& p = out_.plugin;
    if (!type) return;
    if (type->isScalar()) {
        if (init && init->constant && init->type()) {
            const Type* it = init->type();
            InitialValue v;
            v.offset = base;
            v.kind = type->kind;
            v.value = (it->isFloating()) ? init->constValue : static_cast<double>(init->constInt);
            if (v.value != 0.0 || std::signbit(v.value)) p.initial.push_back(v);
        }
        return;
    }
    if (type->kind == TypeKind::Array) {
        if (init && init->kind == ExprKind::InitList) {
            auto* list = static_cast<InitListExpr*>(init);
            for (size_t i = 0; i < list->items.size(); ++i)
                initialValues(type->element, base + static_cast<int>(i) * type->element->size,
                              list->items[i].value, scope);
            for (int i = static_cast<int>(list->items.size()); i < type->count; ++i)
                initialValues(type->element, base + i * type->element->size, nullptr, scope);
        } else {
            for (int i = 0; i < type->count; ++i)
                initialValues(type->element, base + i * type->element->size, nullptr, scope);
        }
        // The spare element keeps its type's defaults too.
        initialValues(type->element, base + type->count * type->element->size, nullptr, scope);
        return;
    }
    if (type->kind == TypeKind::Struct) {
        StructDecl* s = type->structDecl;
        std::map<std::string, Expr*> given;
        if (init && init->kind == ExprKind::InitList)
            for (auto& item : static_cast<InitListExpr*>(init)->items)
                given[item.designator] = item.value;
        for (FieldDecl* f : s->fields) {
            if (f->isStatic || !f->qt.type) continue;
            auto it = given.find(f->name);
            Expr* value = it != given.end() ? it->second : f->init;
            if (f->qt.type->kind == TypeKind::Object && f->qt.type->builtin == b_.param()) {
                if (f->paramIndex >= 0) {
                    const ParamModel& pm = p.params[static_cast<size_t>(f->paramIndex)];
                    ParamSlot slot;
                    slot.offset = base + f->offset;
                    slot.index = f->paramIndex;
                    slot.def = pm.kind == ParamModel::Kind::Continuous &&
                                       pm.unit == ParamModel::Unit::Percent
                                   ? pm.def * 0.01
                                   : pm.def;
                    slot.min = pm.unit == ParamModel::Unit::Percent ? pm.min * 0.01 : pm.min;
                    slot.offAtMinimum = pm.offAtMinimum;
                    p.paramSlots.push_back(slot);
                }
                continue;
            }
            initialValues(f->qt.type, base + f->offset, value, s);
        }
        return;
    }
}

void Sema::buildConstruction() {
    PluginModel& p = out_.plugin;
    if (!p.decl || !p.decl->type) return;
    p.size = p.decl->type->size;
    p.align = std::max(8, p.decl->type->align);
    p.initial.clear();
    p.objects.clear();
    p.paramSlots.clear();
    initialValues(p.decl->type, 0, nullptr, p.decl);
    collectObjects(p.decl->type, 0, p.objects);
}

// --- whole program -----------------------------------------------------------------------

void Sema::contexts() {
    PluginModel& p = out_.plugin;
    std::function<void(FunctionDecl*, std::uint8_t)> mark = [&](FunctionDecl* f, std::uint8_t ctx) {
        if (!f || (f->contexts & ctx) == ctx) return;
        f->contexts |= ctx;
        for (FunctionDecl* g : f->calls) mark(g, ctx);
    };
    mark(p.prepare, kInit);
    mark(p.update, kAudio);
    mark(p.process, kAudio);
    mark(p.startVoice, kAudio);
    mark(p.startVoiceRetrigger, kAudio);
    mark(p.releaseVoice, kAudio);
    mark(p.stopVoice, kAudio);
    // Recursion: a cycle in the call graph.
    std::map<FunctionDecl*, int> state;  // 1: on the stack, 2: done
    std::function<bool(FunctionDecl*)> visit = [&](FunctionDecl* f) -> bool {
        state[f] = 1;
        for (FunctionDecl* g : f->calls) {
            if (state[g] == 1) {
                error(
                    g->loc, "script.call.recursion",
                    g == f ? f->name + "() calls itself; recursion is not in the subset"
                           : f->name + "() and " + g->name +
                                 "() call each other; recursion is not in the subset",
                    "write it as a loop (the depth of a recursion is unbounded, the stack is not)");
                return false;
            }
            if (state[g] == 0 && !visit(g)) return false;
        }
        state[f] = 2;
        return true;
    };
    for (FunctionDecl* f : out_.functions)
        if (state[f] == 0 && !visit(f)) return;
}

// --- effects and evaluation order ---------------------------------------------------------

bool Sema::overlaps(const Access& a, const Access& b) {
    if (a.root == -2 || b.root == -2) return true;
    if (a.root != b.root) return false;
    const size_t n = std::min(a.path.size(), b.path.size());
    for (size_t i = 0; i < n; ++i)
        if (a.path[i] && b.path[i] && a.path[i] != b.path[i]) return false;
    return true;
}

bool Sema::conflict(const Effects& a, const Effects& b) const {
    for (const Access& w : a.writes) {
        for (const Access& r : b.reads)
            if (overlaps(w, r)) return true;
        for (const Access& x : b.writes)
            if (overlaps(w, x)) return true;
    }
    for (const Access& w : b.writes)
        for (const Access& r : a.reads)
            if (overlaps(w, r)) return true;
    return false;
}

// The storage an lvalue designates.
bool Sema::storage(Expr* e, Access& out) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Name: {
            auto* n = static_cast<NameExpr*>(e);
            if (n->ref == RefKind::Local && n->var) {
                if (n->var->qt.isRef && !n->var->isParam) {
                    // A reference local is what it was bound to; unknown
                    // storage (overlapping everything) if that is unknown.
                    auto it = aliases_.find(n->var);
                    if (it != aliases_.end()) {
                        out = it->second;
                        return true;
                    }
                    out.root = -2;
                    return true;
                }
                // A reference parameter stays the parameter: the function's
                // summary maps it to the caller's argument.
                out.root = n->var->id;
                return true;
            }
            if (n->ref == RefKind::Field) {
                out.root = -1;
                out.path = {n->field};
                return true;
            }
            return false;
        }
        case ExprKind::Member: {
            auto* m = static_cast<MemberExpr*>(e);
            if (!storage(m->object, out)) return false;
            if (m->ref == RefKind::Field)
                out.path.push_back(m->field);
            else if (m->ref == RefKind::BuiltinField)
                out.path.push_back(nullptr);
            return true;
        }
        case ExprKind::Index: {
            auto* x = static_cast<IndexExpr*>(e);
            const Type* ot = x->object->type();
            if (ot && ot->kind == TypeKind::Object) {
                // io[c][i]: the audio, which only a Block reaches.
                out.root = -3;
                return true;
            }
            if (!storage(x->object, out)) return false;
            out.path.push_back(nullptr);
            return true;
        }
        case ExprKind::Cast:
            return storage(static_cast<CastExpr*>(e)->operand, out);
        default:
            return false;
    }
}

Sema::Effects Sema::effectsOf(Expr* e) {
    Effects fx;
    if (!e) return fx;
    auto merge = [&](const Effects& x) {
        fx.reads.insert(fx.reads.end(), x.reads.begin(), x.reads.end());
        fx.writes.insert(fx.writes.end(), x.writes.begin(), x.writes.end());
    };
    auto read = [&](Expr* x) {
        Access a;
        if (storage(x, a)) fx.reads.push_back(a);
    };
    auto write = [&](Expr* x) {
        Access a;
        if (storage(x, a)) {
            a.write = true;
            fx.writes.push_back(a);
        } else {
            fx.writes.push_back(Access{-2, {}, true});
        }
    };
    switch (e->kind) {
        case ExprKind::Name:
        case ExprKind::Member:
            read(e);
            if (e->kind == ExprKind::Member) merge(effectsOf(static_cast<MemberExpr*>(e)->object));
            break;
        case ExprKind::Index: {
            auto* x = static_cast<IndexExpr*>(e);
            read(e);
            merge(effectsOf(x->object));
            merge(effectsOf(x->index));
            break;
        }
        case ExprKind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            merge(effectsOf(u->operand));
            if (u->op == "++" || u->op == "--") write(u->operand);
            break;
        }
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            merge(effectsOf(b->left));
            merge(effectsOf(b->right));
            break;
        }
        case ExprKind::Assign: {
            auto* a = static_cast<AssignExpr*>(e);
            merge(effectsOf(a->value));
            merge(effectsOf(a->target));
            write(a->target);
            break;
        }
        case ExprKind::Conditional: {
            auto* c = static_cast<ConditionalExpr*>(e);
            merge(effectsOf(c->cond));
            merge(effectsOf(c->then));
            merge(effectsOf(c->otherwise));
            break;
        }
        case ExprKind::Cast:
            merge(effectsOf(static_cast<CastExpr*>(e)->operand));
            break;
        case ExprKind::InitList:
            for (auto& item : static_cast<InitListExpr*>(e)->items) merge(effectsOf(item.value));
            break;
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if (c->object) merge(effectsOf(c->object));
            for (Expr* a : c->args) merge(effectsOf(a));
            if (c->function) {
                const Summary& s = summaries_[c->function];
                Access base;
                bool known = false;
                if (c->object)
                    known = storage(c->object, base);
                else if (c->implicitThis) {
                    base.root = -1;
                    known = true;
                }
                // What the callee reads of its object: all of it.
                if (known && (c->object || c->implicitThis)) fx.reads.push_back(base);
                if (s.writesAllOfThis || (!s.writesFields.empty() && !known)) {
                    Access w = known ? base : Access{-2, {}, true};
                    w.write = true;
                    if (c->object || c->implicitThis) fx.writes.push_back(w);
                } else if (known) {
                    for (const FieldDecl* f : s.writesFields) {
                        Access w = base;
                        w.path.push_back(f);
                        w.write = true;
                        fx.writes.push_back(w);
                    }
                }
                for (int i : s.writesParams)
                    if (static_cast<size_t>(i) < c->args.size())
                        write(c->args[static_cast<size_t>(i)]);
            } else if (c->builtinMethod) {
                if (c->builtinMethod->writes == Writes::Self) {
                    if (c->object)
                        write(c->object);
                    else if (c->implicitThis)
                        fx.writes.push_back(Access{-1, {}, true});
                } else if (c->object) {
                    read(c->object);
                }
            } else if (c->object && c->callee->kind == ExprKind::Member) {
                // Array/Buffer fill() and resize() change the container.
                const std::string& m = static_cast<MemberExpr*>(c->callee)->member;
                if (m == "fill" || m == "resize")
                    write(c->object);
                else
                    read(c->object);
            }
            break;
        }
        default:
            break;
    }
    return fx;
}

// What each reference local refers to, for the evaluation-order rule.
void Sema::aliasesIn(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Compound:
            for (Stmt* x : static_cast<CompoundStmt*>(s)->body) aliasesIn(x);
            return;
        case StmtKind::Decl:
            for (VarDecl* v : static_cast<DeclStmt*>(s)->vars)
                if (v->qt.isRef && v->init) {
                    Access a;
                    if (storage(v->init, a)) aliases_[v] = a;
                }
            return;
        case StmtKind::If:
            aliasesIn(static_cast<IfStmt*>(s)->then);
            aliasesIn(static_cast<IfStmt*>(s)->otherwise);
            return;
        case StmtKind::For:
            aliasesIn(static_cast<ForStmt*>(s)->init);
            aliasesIn(static_cast<ForStmt*>(s)->body);
            return;
        case StmtKind::RangeFor: {
            auto* r = static_cast<RangeForStmt*>(s);
            if (r->var && r->var->qt.isRef) {
                Access a;
                if (storage(r->range, a)) {
                    a.path.push_back(nullptr);  // some element
                    aliases_[r->var] = a;
                }
            }
            aliasesIn(r->body);
            return;
        }
        case StmtKind::While:
            aliasesIn(static_cast<WhileStmt*>(s)->body);
            return;
        case StmtKind::DoWhile:
            aliasesIn(static_cast<DoWhileStmt*>(s)->body);
            return;
        case StmtKind::Switch:
            aliasesIn(static_cast<SwitchStmt*>(s)->body);
            return;
        default:
            return;
    }
}

void Sema::effects() {
    for (FunctionDecl* f : out_.functions) aliasesIn(f->body);
    // Summaries, callees first (the call graph has no cycles by now).
    std::set<FunctionDecl*> done;
    std::function<void(FunctionDecl*)> summarise = [&](FunctionDecl* f) {
        if (done.count(f)) return;
        done.insert(f);
        for (FunctionDecl* g : f->calls) summarise(g);
        Summary s;
        std::function<void(Stmt*)> walkStmt;
        std::function<void(Expr*)> walk = [&](Expr* e) {
            if (!e) return;
            const Effects fx = effectsOf(e);
            for (const Access& w : fx.writes) {
                if (w.root == -1) {
                    if (w.path.empty() || !w.path[0])
                        s.writesAllOfThis = true;
                    else
                        s.writesFields.insert(static_cast<const FieldDecl*>(w.path[0]));
                } else if (w.root == -2) {
                    s.writesAllOfThis = true;  // unknown: assume the worst
                    for (size_t i = 0; i < f->params.size(); ++i)
                        if (f->params[i]->qt.isRef && !f->params[i]->qt.isConst)
                            s.writesParams.insert(static_cast<int>(i));
                } else if (w.root >= 0 && static_cast<size_t>(w.root) < f->params.size() &&
                           f->params[static_cast<size_t>(w.root)]->qt.isRef) {
                    s.writesParams.insert(w.root);
                }
            }
        };
        walkStmt = [&](Stmt* st) {
            if (!st) return;
            switch (st->kind) {
                case StmtKind::Compound:
                    for (Stmt* x : static_cast<CompoundStmt*>(st)->body) walkStmt(x);
                    break;
                case StmtKind::Expr:
                    walk(static_cast<ExprStmt*>(st)->expr);
                    break;
                case StmtKind::Decl:
                    for (VarDecl* v : static_cast<DeclStmt*>(st)->vars) walk(v->init);
                    break;
                case StmtKind::If:
                    walk(static_cast<IfStmt*>(st)->cond);
                    walkStmt(static_cast<IfStmt*>(st)->then);
                    walkStmt(static_cast<IfStmt*>(st)->otherwise);
                    break;
                case StmtKind::For:
                    walkStmt(static_cast<ForStmt*>(st)->init);
                    walk(static_cast<ForStmt*>(st)->cond);
                    walk(static_cast<ForStmt*>(st)->step);
                    walkStmt(static_cast<ForStmt*>(st)->body);
                    break;
                case StmtKind::RangeFor:
                    walk(static_cast<RangeForStmt*>(st)->range);
                    walkStmt(static_cast<RangeForStmt*>(st)->body);
                    // Writing through the loop's reference writes the range.
                    if (static_cast<RangeForStmt*>(st)->var &&
                        static_cast<RangeForStmt*>(st)->var->qt.isRef) {
                        Access a;
                        if (storage(static_cast<RangeForStmt*>(st)->range, a)) {
                            if (a.root == -1) {
                                if (a.path.empty())
                                    s.writesAllOfThis = true;
                                else
                                    s.writesFields.insert(static_cast<const FieldDecl*>(a.path[0]));
                            }
                        }
                    }
                    break;
                case StmtKind::While:
                    walk(static_cast<WhileStmt*>(st)->cond);
                    walkStmt(static_cast<WhileStmt*>(st)->body);
                    break;
                case StmtKind::DoWhile:
                    walkStmt(static_cast<DoWhileStmt*>(st)->body);
                    walk(static_cast<DoWhileStmt*>(st)->cond);
                    break;
                case StmtKind::Switch:
                    walk(static_cast<SwitchStmt*>(st)->cond);
                    walkStmt(static_cast<SwitchStmt*>(st)->body);
                    break;
                case StmtKind::Return:
                    walk(static_cast<ReturnStmt*>(st)->value);
                    break;
                default:
                    break;
            }
        };
        // References bound in this function may write what they refer to;
        // a reference local makes its writes unknown (root -2), handled above.
        walkStmt(f->body);
        summaries_[f] = s;
    };
    for (FunctionDecl* f : out_.functions) summarise(f);
}

void Sema::sequencingIn(Expr* e) {
    if (!e) return;
    auto report = [&](Loc loc) {
        error(loc, "script.ub.order",
              "C++ does not define the order in which these operands are evaluated, and one of "
              "them changes "
              "something the other uses",
              "split it into separate statements: store one result in a local first");
    };
    switch (e->kind) {
        case ExprKind::Binary: {
            auto* b = static_cast<BinaryExpr*>(e);
            sequencingIn(b->left);
            sequencingIn(b->right);
            // &&, ||, the comma and the shifts are sequenced left to right.
            if (b->op == "&&" || b->op == "||" || b->op == "," || b->op == "<<" || b->op == ">>")
                return;
            if (conflict(effectsOf(b->left), effectsOf(b->right))) report(b->loc);
            return;
        }
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            sequencingIn(c->object);
            for (Expr* a : c->args) sequencingIn(a);
            std::vector<Effects> fx;
            for (Expr* a : c->args) fx.push_back(effectsOf(a));
            for (size_t i = 0; i < fx.size(); ++i)
                for (size_t j = i + 1; j < fx.size(); ++j)
                    if (conflict(fx[i], fx[j])) {
                        report(c->args[j]->loc);
                        return;
                    }
            return;
        }
        case ExprKind::Unary:
            sequencingIn(static_cast<UnaryExpr*>(e)->operand);
            return;
        case ExprKind::Assign:
            sequencingIn(static_cast<AssignExpr*>(e)->target);
            sequencingIn(static_cast<AssignExpr*>(e)->value);
            return;
        case ExprKind::Conditional:
            sequencingIn(static_cast<ConditionalExpr*>(e)->cond);
            sequencingIn(static_cast<ConditionalExpr*>(e)->then);
            sequencingIn(static_cast<ConditionalExpr*>(e)->otherwise);
            return;
        case ExprKind::Cast:
            sequencingIn(static_cast<CastExpr*>(e)->operand);
            return;
        case ExprKind::Index:
            sequencingIn(static_cast<IndexExpr*>(e)->object);
            sequencingIn(static_cast<IndexExpr*>(e)->index);
            return;
        case ExprKind::Member:
            sequencingIn(static_cast<MemberExpr*>(e)->object);
            return;
        case ExprKind::InitList:
            for (auto& item : static_cast<InitListExpr*>(e)->items) sequencingIn(item.value);
            return;
        default:
            return;
    }
}

void Sema::sequencingIn(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Compound:
            for (Stmt* x : static_cast<CompoundStmt*>(s)->body) sequencingIn(x);
            return;
        case StmtKind::Expr:
            sequencingIn(static_cast<ExprStmt*>(s)->expr);
            return;
        case StmtKind::Decl:
            for (VarDecl* v : static_cast<DeclStmt*>(s)->vars) sequencingIn(v->init);
            return;
        case StmtKind::If:
            sequencingIn(static_cast<IfStmt*>(s)->cond);
            sequencingIn(static_cast<IfStmt*>(s)->then);
            sequencingIn(static_cast<IfStmt*>(s)->otherwise);
            return;
        case StmtKind::For:
            sequencingIn(static_cast<ForStmt*>(s)->init);
            sequencingIn(static_cast<ForStmt*>(s)->cond);
            sequencingIn(static_cast<ForStmt*>(s)->step);
            sequencingIn(static_cast<ForStmt*>(s)->body);
            return;
        case StmtKind::RangeFor:
            sequencingIn(static_cast<RangeForStmt*>(s)->range);
            sequencingIn(static_cast<RangeForStmt*>(s)->body);
            return;
        case StmtKind::While:
            sequencingIn(static_cast<WhileStmt*>(s)->cond);
            sequencingIn(static_cast<WhileStmt*>(s)->body);
            return;
        case StmtKind::DoWhile:
            sequencingIn(static_cast<DoWhileStmt*>(s)->body);
            sequencingIn(static_cast<DoWhileStmt*>(s)->cond);
            return;
        case StmtKind::Switch:
            sequencingIn(static_cast<SwitchStmt*>(s)->cond);
            sequencingIn(static_cast<SwitchStmt*>(s)->body);
            return;
        case StmtKind::Return:
            sequencingIn(static_cast<ReturnStmt*>(s)->value);
            return;
        default:
            return;
    }
}

void Sema::sequencing() {
    for (FunctionDecl* f : out_.functions) {
        fn_ = f;
        sequencingIn(f->body);
    }
    fn_ = nullptr;
}

// Real-time rules: what audio code may not call.
void Sema::checkRules() {
    for (FunctionDecl* f : out_.functions) {
        if (!(f->contexts & kAudio)) continue;
        std::function<void(Expr*)> walk;
        std::function<void(Stmt*)> walkStmt;
        walk = [&](Expr* e) {
            if (!e) return;
            switch (e->kind) {
                case ExprKind::Call: {
                    auto* c = static_cast<CallExpr*>(e);
                    if (c->builtinMethod && !(c->builtinMethod->contexts & kAudio))
                        error(c->loc, "script.rt.allocation",
                              c->builtinMethod->owner->name + "::" + c->builtinMethod->name +
                                  "() allocates memory, which audio code (" + f->name +
                                  "(), called from process() or "
                                  "update()) may not do",
                              "call it in prepare(), where the sample rate is known and allocating "
                              "is allowed");
                    if (c->builtin && !(c->builtin->contexts & kAudio))
                        error(c->loc, "script.rt.allocation",
                              c->builtin->name + "() is not available in audio code",
                              "call it in prepare()");
                    if (c->callee->kind == ExprKind::Member &&
                        static_cast<MemberExpr*>(c->callee)->member == "resize" && c->object &&
                        c->object->type() && c->object->type()->kind == TypeKind::Buffer)
                        error(c->loc, "script.rt.allocation",
                              "resize() allocates memory, which audio code (" + f->name +
                                  "()) may not do",
                              "size the Buffer in prepare(); in process() use the size it has");
                    if (c->object) walk(c->object);
                    for (Expr* a : c->args) walk(a);
                    return;
                }
                case ExprKind::Unary:
                    walk(static_cast<UnaryExpr*>(e)->operand);
                    return;
                case ExprKind::Binary:
                    walk(static_cast<BinaryExpr*>(e)->left);
                    walk(static_cast<BinaryExpr*>(e)->right);
                    return;
                case ExprKind::Assign:
                    walk(static_cast<AssignExpr*>(e)->target);
                    walk(static_cast<AssignExpr*>(e)->value);
                    return;
                case ExprKind::Conditional:
                    walk(static_cast<ConditionalExpr*>(e)->cond);
                    walk(static_cast<ConditionalExpr*>(e)->then);
                    walk(static_cast<ConditionalExpr*>(e)->otherwise);
                    return;
                case ExprKind::Cast:
                    walk(static_cast<CastExpr*>(e)->operand);
                    return;
                case ExprKind::Index:
                    walk(static_cast<IndexExpr*>(e)->object);
                    walk(static_cast<IndexExpr*>(e)->index);
                    return;
                case ExprKind::Member:
                    walk(static_cast<MemberExpr*>(e)->object);
                    return;
                case ExprKind::InitList:
                    for (auto& item : static_cast<InitListExpr*>(e)->items) walk(item.value);
                    return;
                default:
                    return;
            }
        };
        walkStmt = [&](Stmt* s) {
            if (!s) return;
            switch (s->kind) {
                case StmtKind::Compound:
                    for (Stmt* x : static_cast<CompoundStmt*>(s)->body) walkStmt(x);
                    return;
                case StmtKind::Expr:
                    walk(static_cast<ExprStmt*>(s)->expr);
                    return;
                case StmtKind::Decl:
                    for (VarDecl* v : static_cast<DeclStmt*>(s)->vars) walk(v->init);
                    return;
                case StmtKind::If:
                    walk(static_cast<IfStmt*>(s)->cond);
                    walkStmt(static_cast<IfStmt*>(s)->then);
                    walkStmt(static_cast<IfStmt*>(s)->otherwise);
                    return;
                case StmtKind::For:
                    walkStmt(static_cast<ForStmt*>(s)->init);
                    walk(static_cast<ForStmt*>(s)->cond);
                    walk(static_cast<ForStmt*>(s)->step);
                    walkStmt(static_cast<ForStmt*>(s)->body);
                    return;
                case StmtKind::RangeFor:
                    walk(static_cast<RangeForStmt*>(s)->range);
                    walkStmt(static_cast<RangeForStmt*>(s)->body);
                    return;
                case StmtKind::While:
                    walk(static_cast<WhileStmt*>(s)->cond);
                    walkStmt(static_cast<WhileStmt*>(s)->body);
                    return;
                case StmtKind::DoWhile:
                    walkStmt(static_cast<DoWhileStmt*>(s)->body);
                    walk(static_cast<DoWhileStmt*>(s)->cond);
                    return;
                case StmtKind::Switch:
                    walk(static_cast<SwitchStmt*>(s)->cond);
                    walkStmt(static_cast<SwitchStmt*>(s)->body);
                    return;
                case StmtKind::Return:
                    walk(static_cast<ReturnStmt*>(s)->value);
                    return;
                default:
                    return;
            }
        };
        walkStmt(f->body);
    }
}

}  // namespace supple::scriptc
