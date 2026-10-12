// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Expressions: what each name means, each expression's type and value
// category, and the conversions C++ makes, made explicit. The rules are C++'s
// ([conv], [over.ics.rank], [expr.arith.conv]) for the subset's types, so the
// JIT computes what a C++ compiler would; where C++ leaves behaviour
// undefined (float to int out of range, a variable divisor, a shift past the
// width) the script is refused with the defined way to say it.
#include <cmath>
#include <limits>

#include "sema.h"

namespace supple::scriptc {

namespace {

bool sameType(const Type* a, const Type* b) { return a == b; }

std::string article(const std::string& t) {
    return (!t.empty() && std::string("aeiouAEIOU").find(t[0]) != std::string::npos ? "an "
                                                                                    : "a ") +
           t;
}

const char* kToIntHint =
    "C++ leaves a double that does not fit in an int undefined; convert explicitly with toInt(x) "
    "(toward zero), "
    "floorToInt(x), ceilToInt(x) or roundToInt(x)";

}  // namespace

// --- conversions ----------------------------------------------------------------

const Type* Sema::promoted(const Type* t) const {
    if (t->kind == TypeKind::Bool) return b_.intType();
    return t;
}

// The usual arithmetic conversions.
const Type* Sema::arithmeticResult(const Type* a, const Type* b) const {
    if (a->kind == TypeKind::Double || b->kind == TypeKind::Double) return b_.doubleType();
    if (a->kind == TypeKind::Float || b->kind == TypeKind::Float) return b_.floatType();
    return b_.intType();
}

ConversionRank Sema::standardRank(const Type* from, const Type* to) const {
    ConversionRank r;
    if (!from || !to) return r;
    if (from == to) {
        r.rank = ConversionRank::Exact;
        return r;
    }
    if (from->kind == TypeKind::Enum || to->kind == TypeKind::Enum)
        return r;  // scoped: no implicit conversions
    if (!from->isArithmetic() || !to->isArithmetic()) return r;
    if ((from->kind == TypeKind::Bool && to->kind == TypeKind::Int) ||
        (from->kind == TypeKind::Float && to->kind == TypeKind::Double))
        r.rank = ConversionRank::Promotion;
    else
        r.rank = ConversionRank::Conversion;
    return r;
}

ConversionRank Sema::rank(const Expr* from, QualType to) const {
    ConversionRank r;
    const Type* ft = from->type();
    if (!ft || !to.type) return r;
    if (to.isRef) {
        if (!to.isConst) {
            if (from->lvalue && sameType(ft, to.type) && !from->qt.isConst)
                r.rank = ConversionRank::Exact;
            return r;
        }
        if (from->lvalue && sameType(ft, to.type)) {
            r.rank = ConversionRank::Exact;
            return r;
        }
        if (!to.type->isScalar()) return r;
    }
    if (ft->kind == TypeKind::Object && ft->builtin == b_.param() && to.type->isArithmetic()) {
        r = standardRank(b_.doubleType(), to.type);
        if (r.rank != ConversionRank::NotViable) r.user = true;
        return r;
    }
    if (ft->kind == TypeKind::InitList) {
        r.rank = ConversionRank::Exact;  // checked when converted
        return r;
    }
    if (to.type->isScalar()) return standardRank(ft, to.type);
    if (sameType(ft, to.type)) r.rank = ConversionRank::Exact;
    return r;
}

// Whether candidate a is better than b: no argument worse, one better.
bool Sema::better(const std::vector<ConversionRank>& a,
                  const std::vector<ConversionRank>& b) const {
    auto cmp = [](const ConversionRank& x, const ConversionRank& y) {
        // -1: x better, 1: y better, 0: neither.
        if (x.user != y.user)
            return x.user ? 1 : -1;  // a standard sequence beats a user-defined one
        if (x.rank != y.rank) return x.rank < y.rank ? -1 : 1;
        return 0;
    };
    bool someBetter = false;
    for (size_t i = 0; i < a.size(); ++i) {
        const int c = cmp(a[i], b[i]);
        if (c > 0) return false;
        if (c < 0) someBetter = true;
    }
    return someBetter;
}

Expr* Sema::implicitCast(Expr* e, const Type* to, bool user) {
    auto* c = unit_.expr<CastExpr>(e->loc);
    c->cast = CastKind::Implicit;
    c->operand = e;
    c->userConversion = user;
    c->qt = QualType{to, false, false};
    c->lvalue = false;
    if (e->constant && !user) {
        c->constant = true;
        if (to->kind == TypeKind::Double)
            c->constValue =
                e->type()->isFloating() ? e->constValue : static_cast<double>(e->constInt);
        else if (to->kind == TypeKind::Float)
            c->constValue =
                static_cast<double>(e->type()->isFloating() ? static_cast<float>(e->constValue)
                                                            : static_cast<float>(e->constInt));
        else if (to->kind == TypeKind::Bool)
            c->constInt = e->type()->isFloating() ? (e->constValue != 0.0) : (e->constInt != 0);
        else if (to->kind == TypeKind::Int && e->type()->isIntegral())
            c->constInt = e->constInt;
        else
            c->constant = false;
        if (c->constant && to->isFloating()) c->constInt = 0;
        if (c->constant && to->isIntegral()) c->constValue = static_cast<double>(c->constInt);
    }
    return c;
}

Expr* Sema::rvalue(Expr* e) { return e; }

Expr* Sema::convert(Expr* e, QualType to, Loc loc, const std::string& what, bool braced) {
    if (!e || !e->type() || !to.type) return e;
    const Type* ft = e->type();
    const std::string toName = typeName(to);
    // Braces.
    if (ft->kind == TypeKind::InitList) {
        auto* list = static_cast<InitListExpr*>(e);
        if (to.isRef && !to.isConst) {
            error(loc, "script.type.reference",
                  "a braced list cannot bind to a non-const reference", "");
            return e;
        }
        const Type* t = to.type;
        if (t->isScalar()) {
            if (list->items.empty()) {
                // {} is zero.
                auto* z = unit_.expr<IntLitExpr>(e->loc);
                z->value = 0;
                z->qt = QualType{b_.intType(), false, false};
                z->constant = true;
                if (t->kind == TypeKind::Enum) {
                    auto* c = unit_.expr<CastExpr>(e->loc);
                    c->cast = CastKind::Static;
                    c->operand = z;
                    c->qt = QualType{t, false, false};
                    c->constant = true;
                    return c;
                }
                return convert(z, QualType{t, false, false}, loc, what, true);
            }
            if (list->items.size() > 1 || !list->items[0].designator.empty()) {
                error(loc, "script.type.init_list",
                      what + ": " + article(t->name) + " takes one value in braces", "");
                return e;
            }
            Expr* v = check(list->items[0].value);
            return convert(v, QualType{t, false, false}, loc, what, true);
        }
        list->qt = QualType{t, false, false};
        if (t->kind == TypeKind::Array) {
            if (static_cast<int>(list->items.size()) > t->count)
                error(loc, "script.type.init_list",
                      what + ": " + std::to_string(list->items.size()) + " values for " + t->name,
                      "");
            for (auto& item : list->items) {
                if (!item.designator.empty())
                    error(item.loc, "script.type.designator", "an Array's elements are not named",
                          "list the values in order");
                item.value = convert(check(item.value), QualType{t->element, false, false},
                                     item.loc, "an element of " + t->name, true);
            }
            return list;
        }
        if (t->kind == TypeKind::Struct) {
            StructDecl* s = t->structDecl;
            std::vector<FieldDecl*> fields;
            for (FieldDecl* f : s->fields)
                if (!f->isStatic) fields.push_back(f);
            size_t next = 0;
            for (auto& item : list->items) {
                FieldDecl* target = nullptr;
                if (!item.designator.empty()) {
                    size_t k = next;
                    while (k < fields.size() && fields[k]->name != item.designator) ++k;
                    if (k == fields.size()) {
                        bool earlier = false;
                        for (size_t j = 0; j < next; ++j)
                            earlier |= fields[j]->name == item.designator;
                        if (earlier)
                            error(item.loc, "script.type.designator_order",
                                  "designated initializers must follow the order " + s->name +
                                      " declares its members",
                                  "put ." + item.designator + " earlier in the braces");
                        else
                            error(item.loc, "script.type.designator",
                                  s->name + " has no member '" + item.designator + "'", "");
                        continue;
                    }
                    next = k;
                } else if (next >= fields.size()) {
                    error(item.loc, "script.type.init_list", "too many values for " + s->name, "");
                    continue;
                }
                target = fields[next++];
                if (target->qt.type && target->qt.type->kind == TypeKind::Object &&
                    target->qt.type->builtin && !target->qt.type->builtin->declarable) {
                    error(item.loc, "script.type.init_list",
                          "'" + target->name + "' cannot be set in braces", "");
                    continue;
                }
                item.designator = target->name;
                item.value = convert(check(item.value), target->qt, item.loc,
                                     "member '" + target->name + "'", true);
            }
            return list;
        }
        if (t->kind == TypeKind::Object || t->kind == TypeKind::Buffer) {
            if (!list->items.empty())
                error(loc, "script.type.init_list", t->name + " is not set from values in braces",
                      "write " + t->name + " name; and set it up with its member functions");
            return list;
        }
        error(loc, "script.type.init_list", what + ": braces cannot make " + article(t->name), "");
        return e;
    }

    if (ft->kind == TypeKind::String) {
        error(loc, "script.type.string",
              what + ": text is only used in declarations (parameter names, Info)",
              "a script computes with numbers");
        return e;
    }
    if (ft->kind == TypeKind::Null) {
        error(loc, "script.unsupported.pointer", "nullptr: pointers are not in the subset", "");
        return e;
    }
    if (ft->isVoid()) {
        error(loc, "script.type.void",
              what + ": this expression has no value (it calls a function returning void)", "");
        return e;
    }

    // References.
    if (to.isRef) {
        if (e->lvalue && sameType(ft, to.type)) {
            if (!to.isConst && e->qt.isConst) {
                error(loc, "script.type.const_reference",
                      what + ": cannot bind a non-const reference to something read-only",
                      "declare the reference const (const " + to.type->name +
                          "&), or the variable without const");
            }
            markAddressTaken(e);
            return e;
        }
        if (!to.isConst) {
            if (!e->lvalue)
                error(loc, "script.type.reference",
                      what + ": a non-const reference needs a variable or a member to refer to",
                      "store the value in a variable first, or take the parameter as const " +
                          to.type->name + "&");
            else
                error(loc, "script.type.reference",
                      what + ": cannot bind " + typeName(QualType{to.type, false, true}) + " to " +
                          article(ft->name),
                      "");
            return e;
        }
        if (!to.type->isScalar()) {
            error(loc, "script.type.reference",
                  what + ": cannot bind " + toName + " to " + article(ft->name), "");
            return e;
        }
        // const T& to a converted value: C++ makes a temporary.
        return convert(e, QualType{to.type, false, false}, loc, what, braced);
    }

    const Type* t = to.type;
    // Param's operator double.
    if (ft->kind == TypeKind::Object && ft->builtin == b_.param()) {
        if (!t->isArithmetic()) {
            error(loc, "script.type.param", what + ": a parameter converts only to a number", "");
            return e;
        }
        if (t->kind == TypeKind::Int) {
            error(loc, "script.type.float_to_int",
                  what + ": a parameter is a double; converting it to int needs care",
                  std::string("for a choice or an integer parameter use .index(); otherwise ") +
                      kToIntHint);
            return e;
        }
        if (braced && t->kind != TypeKind::Double) {
            error(loc, "script.type.narrowing",
                  what + ": converting a parameter (a double) to " + t->name +
                      " in braces is a narrowing conversion",
                  "use = instead of braces, or " + t->name + "(x)");
            return e;
        }
        Expr* asDouble = implicitCast(e, b_.doubleType(), true);
        return t->kind == TypeKind::Double ? asDouble : implicitCast(asDouble, t);
    }
    if (t->isScalar()) {
        if (!ft->isScalar()) {
            error(loc, "script.type.mismatch",
                  what + ": expected " + article(t->name) + ", not " + article(ft->name), "");
            return e;
        }
        if (ft == t) return e;
        if (ft->kind == TypeKind::Enum || t->kind == TypeKind::Enum) {
            error(loc, "script.type.enum",
                  what + ": " + ft->name + " does not convert to " + t->name + " implicitly",
                  t->kind == TypeKind::Enum ? "name a value: " + t->name + "::..."
                                            : "convert explicitly: static_cast<int>(value)");
            return e;
        }
        if (ft->isFloating() && t->kind == TypeKind::Int) {
            error(loc, "script.type.float_to_int", what + ": converting " + ft->name + " to int",
                  kToIntHint);
            return e;
        }
        if (braced) {
            // List-initialization refuses narrowing ([dcl.init.list]).
            bool narrowing = false;
            if (ft->kind == TypeKind::Double && t->kind == TypeKind::Float)
                narrowing =
                    !(e->constant && std::fabs(e->constValue) <=
                                         static_cast<double>(std::numeric_limits<float>::max()));
            else if (ft->kind == TypeKind::Int && t->isFloating())
                narrowing =
                    !(e->constant && static_cast<double>(e->constInt) ==
                                         (t->kind == TypeKind::Float
                                              ? static_cast<double>(static_cast<float>(e->constInt))
                                              : static_cast<double>(e->constInt)));
            else if (ft->kind == TypeKind::Int && t->kind == TypeKind::Bool)
                narrowing = !(e->constant && (e->constInt == 0 || e->constInt == 1));
            else if (ft->isFloating() && t->kind == TypeKind::Bool)
                narrowing = true;
            if (narrowing) {
                error(loc, "script.type.narrowing",
                      what + ": converting " + ft->name + " to " + t->name +
                          " in braces is a narrowing conversion",
                      "use = instead of braces, or convert explicitly: " + t->name + "(x)");
                return e;
            }
        }
        return implicitCast(e, t);
    }
    // Class types: a copy of the same type.
    if (!sameType(ft, t)) {
        error(loc, "script.type.mismatch",
              what + ": expected " + article(t->name) + ", not " + article(ft->name), "");
        return e;
    }
    if (!copyable(t)) {
        error(loc, "script.type.copy",
              what + ": " + t->name + " holds memory and cannot be copied (a copy would allocate)",
              "refer to it instead: auto& name = ...;");
        return e;
    }
    return e;
}

Expr* Sema::condition(Expr* e) {
    e = check(e);
    if (!e || !e->type()) return e;
    if (e->type()->kind == TypeKind::Enum) {
        error(e->loc, "script.type.enum", "a scoped enum is not a condition",
              "compare it: mode == Mode::Value");
        return e;
    }
    return convert(e, QualType{b_.boolType(), false, false}, e->loc, "the condition");
}

void Sema::markAddressTaken(Expr* e) {
    if (e->kind == ExprKind::Name) {
        auto* n = static_cast<NameExpr*>(e);
        if (n->var) n->var->addressTaken = true;
    }
}

bool Sema::requireModifiable(Expr* e, Loc loc, const std::string& what) {
    if (!e->type()) return false;
    if (!e->lvalue) {
        error(loc, "script.type.not_assignable", what + " needs a variable or a member to change",
              "");
        return false;
    }
    if (e->type()->kind == TypeKind::Object && e->type()->builtin == b_.param()) {
        error(loc, "script.param.read_only", what + ": parameters are read-only in a script",
              "the host, the editor and patches set them; keep a working copy in a member if you "
              "need one");
        return false;
    }
    if (e->qt.isConst) {
        std::string why = "it is const";
        if (fn_ && fn_->isConst) why += " (inside a const member function, members are read-only)";
        error(loc, "script.type.const", what + ": cannot change this, " + why, "");
        return false;
    }
    return true;
}

// --- names ----------------------------------------------------------------------

VarDecl* Sema::lookupLocal(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return nullptr;
}

namespace {
FieldDecl* findField(StructDecl* s, const std::string& name) {
    for (FieldDecl* f : s->fields)
        if (f->name == name) return f;
    return nullptr;
}
FunctionDecl* findMethod(StructDecl* s, const std::string& name) {
    for (FunctionDecl* f : s->methods)
        if (f->name == name) return f;
    return nullptr;
}
}  // namespace

Expr* Sema::checkName(NameExpr* n) {
    const std::string& first = n->path[0];
    auto constantOf = [&](Expr* target, const QualType& qt, const std::optional<Constant>& c) {
        target->qt = qt;
        target->qt.isConst = true;
        target->qt.isRef = false;
        if (c && c->numeric()) {
            target->constant = true;
            target->constValue = c->asDouble();
            target->constInt = c->kind == Constant::Kind::Double
                                   ? 0
                                   : (c->kind == Constant::Kind::Int ? c->intValue : c->boolValue);
        }
    };
    if (n->path.size() == 1) {
        if (VarDecl* v = lookupLocal(first)) {
            n->ref = RefKind::Local;
            n->var = v;
            n->qt = v->qt;
            n->qt.isRef = false;
            n->lvalue = true;
            if (v->qt.isConst && v->init && v->init->constant && v->qt.type &&
                v->qt.type->isScalar() && !v->isParam) {
                n->constant = true;
                n->constValue = v->init->constValue;
                n->constInt = v->init->constInt;
            }
            return n;
        }
        // Members of the struct whose function this is, then its outer structs'
        // static members, then the plugin base.
        for (StructDecl* s = self_; s; s = s->outer) {
            if (FieldDecl* f = findField(s, first)) {
                if (s != self_ && !f->isStatic) {
                    error(n->loc, "script.name.outer_member",
                          "'" + first + "' belongs to " + s->name + ", not to " + self_->name,
                          "pass it to the function as a parameter");
                    return n;
                }
                n->field = f;
                if (f->isStatic) {
                    n->ref = RefKind::StaticConst;
                    constantOf(n, f->qt, evaluate(f->init, s));
                    return n;
                }
                n->ref = RefKind::Field;
                n->qt = f->qt;
                n->qt.isRef = false;
                n->qt.isConst = n->qt.isConst || (fn_ && fn_->isConst);
                n->lvalue = true;
                return n;
            }
            if (FunctionDecl* m = findMethod(s, first)) {
                if (s != self_) {
                    error(n->loc, "script.name.outer_member",
                          "'" + first + "' belongs to " + s->name, "");
                    return n;
                }
                n->ref = RefKind::Method;
                n->function = m;
                return n;
            }
        }
        if (self_ && self_->base != PluginKind::None) {
            for (const auto& m : b_.pluginBase()->methods)
                if (m.name == first) {
                    n->ref = RefKind::BuiltinMethod;
                    return n;  // checkCall resolves it
                }
        }
        if (auto g = globals_.find(first); g != globals_.end()) {
            VarDecl* v = g->second;
            n->ref = RefKind::Global;
            n->var = v;
            constantOf(n, v->qt, v->init ? evaluate(v->init, nullptr) : std::nullopt);
            return n;
        }
        if (auto f = functions_.find(first); f != functions_.end()) {
            n->ref = RefKind::Function;
            n->function = f->second;
            return n;
        }
        if (first == "pi" || first == "twoPi") {
            n->ref = RefKind::Builtin;
            n->qt = QualType{b_.doubleType(), true, false};
            n->constant = true;
            n->constValue = first == "pi" ? 3.14159265358979323846 : 6.28318530717958647692;
            return n;
        }
        auto fns = b_.functions(first);
        if (!fns.empty()) {
            n->ref = RefKind::Builtin;
            n->builtins = fns;
            return n;
        }
        if (findStruct(first, self_) || findEnum(first, self_) || b_.cls(first)) {
            n->ref = RefKind::Type;
            return n;
        }
        std::string hint;
        if (first == "M_PI" || first == "PI")
            hint = "use pi (and twoPi for 2 pi)";
        else if (first == "fabs" || first == "fabsf")
            hint = "use abs(x)";
        else if (first == "fmin" || first == "fminf")
            hint = "use min(a, b)";
        else if (first == "fmax" || first == "fmaxf")
            hint = "use max(a, b)";
        else if (first == "powf" || first == "expf" || first == "sinf" || first == "cosf" ||
                 first == "tanhf" || first == "sqrtf" || first == "logf")
            hint = "use " + first.substr(0, first.size() - 1) + "(x): math works in double";
        else if (first == "getSampleRate" || first == "samplerate" || first == "fs" ||
                 first == "srate")
            hint = "use sampleRate()";
        else if (first == "printf" || first == "cout" || first == "std")
            hint = "a script cannot print; report values with a meter() parameter";
        else if (first == "isnan" || first == "isinf" || first == "isfinite")
            hint = "use isFinite(x)";
        else if (first == "rand" || first == "random")
            hint = "declare a Random member: Random rng; ... rng.next()";
        else if (first == "db2gain" || first == "dB2gain" || first == "decibelsToGain" ||
                 first == "dbtoa")
            hint = "use dbToGain(db)";
        else if (first == "gain2db" || first == "gainToDecibels" || first == "atodb")
            hint = "use gainToDb(gain)";
        else if (first == "midiToHz" || first == "noteToHz" || first == "mtoh")
            hint = "use mtof(note)";
        error(n->loc, "script.name.unknown", "unknown name '" + first + "'", hint);
        return n;
    }

    // Qualified: Enum::Value, Struct::kConstant, Oscillator::Shape::Saw.
    const std::string& last = n->path.back();
    // Find the scope the qualifier names.
    StructDecl* scopeStruct = nullptr;
    EnumDecl* scopeEnum = nullptr;
    const BuiltinClass* scopeClass = nullptr;
    const Type* scopeBuiltinEnum = nullptr;
    for (size_t i = 0; i + 1 < n->path.size(); ++i) {
        const std::string& part = n->path[i];
        if (i == 0) {
            if ((scopeStruct = findStruct(part, self_))) continue;
            if ((scopeEnum = findEnum(part, self_))) continue;
            if ((scopeClass = b_.cls(part))) continue;
            if (part == "supple" || part == "script") continue;
        } else if (scopeStruct) {
            StructDecl* inner = nullptr;
            for (StructDecl* s : scopeStruct->nested)
                if (s->name == part) inner = s;
            if (inner) {
                scopeStruct = inner;
                continue;
            }
            EnumDecl* e = nullptr;
            for (EnumDecl* x : scopeStruct->enums)
                if (x->name == part) e = x;
            if (e) {
                scopeEnum = e;
                scopeStruct = nullptr;
                continue;
            }
        } else if (scopeClass) {
            auto it = scopeClass->nestedTypes.find(part);
            if (it != scopeClass->nestedTypes.end()) {
                scopeBuiltinEnum = it->second;
                scopeClass = nullptr;
                continue;
            }
        } else if (part == "script" || part == "supple") {
            continue;
        }
        error(
            n->loc, "script.name.unknown",
            "unknown name '" + part + "' in '" +
                [&] {
                    std::string s;
                    for (size_t k = 0; k < n->path.size(); ++k) s += (k ? "::" : "") + n->path[k];
                    return s;
                }() +
                "'",
            "");
        return n;
    }
    if (scopeEnum) {
        if (!scopeEnum->type) return n;
        for (size_t k = 0; k < scopeEnum->enumerators.size(); ++k)
            if (scopeEnum->enumerators[k].first == last) {
                n->ref = RefKind::Enumerator;
                n->enumValue = scopeEnum->values[k];
                n->qt = QualType{scopeEnum->type, true, false};
                n->constant = true;
                n->constInt = n->enumValue;
                n->constValue = n->enumValue;
                return n;
            }
        error(n->loc, "script.name.enumerator", scopeEnum->name + " has no value '" + last + "'",
              "");
        return n;
    }
    if (scopeBuiltinEnum) {
        const BuiltinEnum* be = b_.enumOf(scopeBuiltinEnum);
        for (const auto& [name, value] : be->values)
            if (name == last) {
                n->ref = RefKind::Enumerator;
                n->enumValue = value;
                n->qt = QualType{scopeBuiltinEnum, true, false};
                n->constant = true;
                n->constInt = value;
                n->constValue = value;
                return n;
            }
        std::string names;
        for (const auto& [name, value] : be->values) names += (names.empty() ? "" : ", ") + name;
        error(n->loc, "script.name.enumerator",
              scopeBuiltinEnum->name + " has no value '" + last + "'", "its values are " + names);
        return n;
    }
    if (scopeStruct) {
        if (FieldDecl* f = findField(scopeStruct, last); f && f->isStatic) {
            n->ref = RefKind::StaticConst;
            n->field = f;
            constantOf(n, f->qt, evaluate(f->init, scopeStruct));
            return n;
        }
        for (EnumDecl* e : scopeStruct->enums)
            if (e->name == last) {
                n->ref = RefKind::Type;
                return n;
            }
        for (StructDecl* s : scopeStruct->nested)
            if (s->name == last) {
                n->ref = RefKind::Type;
                return n;
            }
        error(n->loc, "script.name.qualified",
              "'" + scopeStruct->name + "::" + last + "' is not a static constant",
              "only static constexpr members and nested types are named with ::");
        return n;
    }
    if (scopeClass) {
        auto it = scopeClass->nestedTypes.find(last);
        if (it != scopeClass->nestedTypes.end()) {
            n->ref = RefKind::Type;
            return n;
        }
        error(n->loc, "script.name.qualified", scopeClass->name + " has no '" + last + "'", "");
        return n;
    }
    // supple::script::name: the runtime's, explicitly.
    NameExpr plain(n->loc);
    n->path = {last};
    return checkName(n);
}

// --- members and indexing -----------------------------------------------------------

Expr* Sema::checkMember(MemberExpr* m) {
    if (m->object->kind == ExprKind::This) {
        // this->x: as plain x, within the member function's struct.
        auto* n = unit_.expr<NameExpr>(m->loc);
        n->path = {m->member};
        if (!self_) {
            error(m->loc, "script.name.this", "'this' outside a member function", "");
            return n;
        }
        if (FieldDecl* f = findField(self_, m->member)) {
            n->field = f;
            if (f->isStatic) {
                n->ref = RefKind::StaticConst;
                n->qt = f->qt;
                return n;
            }
            n->ref = RefKind::Field;
            n->qt = f->qt;
            n->qt.isRef = false;
            n->qt.isConst = n->qt.isConst || (fn_ && fn_->isConst);
            n->lvalue = true;
            return n;
        }
        if (FunctionDecl* fm = findMethod(self_, m->member)) {
            n->ref = RefKind::Method;
            n->function = fm;
            return n;
        }
        error(m->loc, "script.name.member", self_->name + " has no member '" + m->member + "'", "");
        return n;
    }
    m->object = check(m->object);
    const Type* t = m->object->type();
    if (!t) return m;
    const bool constObject = m->object->qt.isConst;
    if (t->kind == TypeKind::Struct) {
        StructDecl* s = t->structDecl;
        if (FieldDecl* f = findField(s, m->member)) {
            m->field = f;
            if (f->isStatic) {
                m->ref = RefKind::StaticConst;
                auto c = evaluate(f->init, s);
                m->qt = QualType{f->qt.type, true, false};
                if (c && c->numeric()) {
                    m->constant = true;
                    m->constValue = c->asDouble();
                    m->constInt = c->kind == Constant::Kind::Int ? c->intValue : c->boolValue;
                }
                return m;
            }
            m->ref = RefKind::Field;
            m->qt = f->qt;
            m->qt.isRef = false;
            m->qt.isConst = f->qt.isConst || constObject;
            m->lvalue = m->object->lvalue;
            if (!m->object->lvalue)
                error(m->loc, "script.type.temporary", "a member of a temporary value",
                      "store the value in a variable first");
            return m;
        }
        if (FunctionDecl* f = findMethod(s, m->member)) {
            m->ref = RefKind::Method;
            m->method = f;
            return m;
        }
        error(m->loc, "script.name.member", s->name + " has no member '" + m->member + "'", "");
        return m;
    }
    if (t->kind == TypeKind::Array || t->kind == TypeKind::Buffer) {
        if (m->member == "size" || m->member == "fill" ||
            (t->kind == TypeKind::Buffer && m->member == "resize")) {
            m->ref = RefKind::BuiltinMethod;
            return m;
        }
        error(m->loc, "script.name.member", t->name + " has no member '" + m->member + "'",
              t->kind == TypeKind::Array
                  ? "an Array has size(), fill(value) and [index]"
                  : "a Buffer has size(), resize(n), fill(value) and [index]");
        return m;
    }
    if (t->kind == TypeKind::Object) {
        const BuiltinClass* c = t->builtin;
        for (size_t i = 0; i < c->fields.size(); ++i)
            if (c->fields[i].name == m->member) {
                m->ref = RefKind::BuiltinField;
                m->builtinField = static_cast<int>(i);
                m->qt = QualType{c->fields[i].type, true, false};
                m->lvalue = m->object->lvalue;
                return m;
            }
        for (const auto& bm : c->methods)
            if (bm.name == m->member) m->builtinMethods.push_back(&bm);
        if (!m->builtinMethods.empty()) {
            m->ref = RefKind::BuiltinMethod;
            return m;
        }
        std::string names;
        for (const auto& f : c->fields) names += (names.empty() ? "" : ", ") + f.name;
        for (const auto& bm : c->methods)
            if (names.find(bm.name + "(") == std::string::npos)
                names += (names.empty() ? "" : ", ") + bm.name + "()";
        error(m->loc, "script.name.member", c->name + " has no member '" + m->member + "'",
              names.empty() ? "" : c->name + " has: " + names);
        return m;
    }
    error(m->loc, "script.name.member", article(t->name) + " has no members", "");
    return m;
}

Expr* Sema::checkIndex(IndexExpr* x) {
    x->object = check(x->object);
    x->index = check(x->index);
    const Type* t = x->object->type();
    if (!t || !x->index->type()) return x;
    const Type* it = x->index->type();
    if (it->kind == TypeKind::Enum)
        error(x->index->loc, "script.type.enum", "an index must be an int, not " + it->name,
              "static_cast<int>(value)");
    else
        x->index =
            convert(x->index, QualType{b_.intType(), false, false}, x->index->loc, "the index");
    if (t->kind == TypeKind::Array || t->kind == TypeKind::Buffer) {
        x->qt = QualType{t->element, x->object->qt.isConst, false};
        x->lvalue = x->object->lvalue;
        if (!x->object->lvalue)
            error(x->loc, "script.type.temporary", "indexing a temporary value",
                  "store it in a variable first");
        return x;
    }
    if (t->kind == TypeKind::Object && t->builtin == b_.block()) {
        x->qt = QualType{b_.channel()->type, false, false};
        x->lvalue = false;
        return x;
    }
    if (t->kind == TypeKind::Object && t->builtin == b_.channel()) {
        // Channel::operator[] is const and returns double&.
        x->qt = QualType{b_.doubleType(), false, false};
        x->lvalue = true;
        return x;
    }
    if (t->kind == TypeKind::Object && t->builtin == b_.inputChannel()) {
        x->qt = QualType{b_.doubleType(), true, false};
        x->lvalue = false;
        return x;
    }
    error(x->loc, "script.type.index", t->name + " cannot be indexed", "");
    return x;
}

// --- calls ------------------------------------------------------------------------

std::vector<Expr*> Sema::callArguments(CallExpr* c, const std::vector<QualType>& params,
                                       const std::vector<Expr*>& defaults,
                                       const std::string& what) {
    std::vector<Expr*> out;
    const size_t required = [&] {
        size_t r = params.size();
        while (r > 0 && r - 1 < defaults.size() && defaults[r - 1]) --r;
        return r;
    }();
    if (c->args.size() < required || c->args.size() > params.size()) {
        error(c->loc, "script.call.arguments",
              what + " takes " +
                  (required == params.size()
                       ? std::to_string(params.size())
                       : std::to_string(required) + " to " + std::to_string(params.size())) +
                  " argument" + (params.size() == 1 ? "" : "s") + ", not " +
                  std::to_string(c->args.size()),
              "");
        return c->args;
    }
    for (size_t i = 0; i < c->args.size(); ++i)
        out.push_back(convert(c->args[i], params[i], c->args[i]->loc,
                              "argument " + std::to_string(i + 1) + " of " + what));
    return out;
}

Expr* Sema::checkCall(CallExpr* c) {
    for (Expr*& a : c->args)
        if (a->kind != ExprKind::InitList)
            a = check(a);
        else
            a->qt = QualType{b_.initListType(), false, false};
    Expr* callee = c->callee;
    std::string calleeName;

    // A member call: object.method(args). (this->method arrives here as a
    // plain name: check() rewrote it.)
    if (callee->kind == ExprKind::Member) {
        {
            auto* m = static_cast<MemberExpr*>(callee);
            c->callee = m;
            calleeName = m->member;
            const Type* ot = m->object->type();
            if (!ot) return c;
            if (m->ref == RefKind::Method) {
                FunctionDecl* f = m->method;
                c->function = f;
                c->object = m->object;
                if (f->isConst == false && m->object->qt.isConst)
                    error(c->loc, "script.call.const",
                          "calling " + f->name + "(), which changes " + ot->name +
                              ", on something read-only",
                          "declare " + f->name + "() const if it changes nothing");
                if (!m->object->lvalue)
                    error(c->loc, "script.type.temporary",
                          "calling a member function on a temporary value", "");
                std::vector<QualType> params;
                std::vector<Expr*> defaults;
                for (VarDecl* p : f->params) {
                    params.push_back(p->qt);
                    defaults.push_back(p->init);
                }
                c->args = callArguments(c, params, defaults, ot->name + "::" + f->name + "()");
                c->qt = f->resultType;
                c->lvalue = f->resultType.isRef;
                c->qt.isRef = false;
                if (fn_) fn_->calls.push_back(f);
                return c;
            }
            if (m->ref == RefKind::BuiltinMethod &&
                (ot->kind == TypeKind::Array || ot->kind == TypeKind::Buffer)) {
                c->object = m->object;
                if (m->member == "size") {
                    if (!c->args.empty())
                        error(c->loc, "script.call.arguments", "size() takes no arguments", "");
                    c->qt = QualType{b_.intType(), false, false};
                    if (ot->kind == TypeKind::Array) {
                        c->constant = true;
                        c->constInt = ot->count;
                        c->constValue = ot->count;
                    }
                    return c;
                }
                if (m->member == "fill") {
                    if (c->args.size() != 1) {
                        error(c->loc, "script.call.arguments", "fill() takes one value", "");
                        return c;
                    }
                    if (m->object->qt.isConst)
                        error(c->loc, "script.call.const", "fill() on something read-only", "");
                    if (!copyable(ot->element))
                        error(c->loc, "script.type.copy",
                              "fill() would copy " + ot->element->name + ", which holds memory",
                              "");
                    c->args[0] = convert(c->args[0], QualType{ot->element, true, true},
                                         c->args[0]->loc, "fill()");
                    c->qt = QualType{b_.voidType(), false, false};
                    return c;
                }
                if (m->member == "resize") {
                    if (c->args.size() != 1) {
                        error(c->loc, "script.call.arguments", "resize() takes the new size", "");
                        return c;
                    }
                    if (m->object->qt.isConst)
                        error(c->loc, "script.call.const", "resize() on something read-only", "");
                    c->args[0] = convert(c->args[0], QualType{b_.intType(), false, false},
                                         c->args[0]->loc, "resize()");
                    c->qt = QualType{b_.voidType(), false, false};
                    return c;
                }
            }
            if (m->ref == RefKind::BuiltinMethod && ot->kind == TypeKind::Object) {
                // Overloads of a runtime method: none today, so the first by
                // arity. Resolution mirrors the free functions' below.
                const BuiltinMethod* chosen = nullptr;
                for (const BuiltinMethod* bm : m->builtinMethods)
                    if (bm->params.size() == c->args.size()) chosen = bm;
                if (!chosen) {
                    const BuiltinMethod* any = m->builtinMethods.front();
                    error(c->loc, "script.call.arguments",
                          ot->name + "::" + any->name + " takes " +
                              std::to_string(any->params.size()) + " argument" +
                              (any->params.size() == 1 ? "" : "s") + ", not " +
                              std::to_string(c->args.size()),
                          any->doc);
                    return c;
                }
                c->builtinMethod = chosen;
                c->object = m->object;
                if (!chosen->isConst && m->object->qt.isConst)
                    error(c->loc, "script.call.const",
                          "calling " + chosen->name + "(), which changes the " + ot->name +
                              ", on something read-only",
                          "");
                if (!m->object->lvalue && !chosen->isConst)
                    error(c->loc, "script.type.temporary",
                          "calling " + chosen->name + "() on a temporary value", "");
                c->args =
                    callArguments(c, chosen->params, {}, ot->name + "::" + chosen->name + "()");
                c->qt = chosen->result;
                c->qt.isRef = false;
                return c;
            }
            if (m->ref == RefKind::BuiltinField) {
                error(c->loc, "script.call.not_function",
                      "'" + m->member + "' is a value, not a function",
                      "write " + m->member + " without ()");
                return c;
            }
            if (m->ref == RefKind::Field) {
                error(c->loc, "script.call.not_function",
                      "'" + m->member + "' is a member, not a function", "");
                return c;
            }
            return c;
        }
    }

    if (callee->kind == ExprKind::Name) {
        auto* n = static_cast<NameExpr*>(callee);
        c->callee = n;
        calleeName = n->path.back();
        switch (n->ref) {
            case RefKind::Function:
            case RefKind::Method: {
                FunctionDecl* f = n->function;
                c->function = f;
                if (n->ref == RefKind::Method) {
                    c->implicitThis = true;
                    if (fn_ && fn_->isConst && !f->isConst)
                        error(c->loc, "script.call.const",
                              "calling " + f->name +
                                  "(), which changes members, from a const member function",
                              "declare " + f->name + "() const if it changes nothing");
                }
                std::vector<QualType> params;
                std::vector<Expr*> defaults;
                for (VarDecl* p : f->params) {
                    params.push_back(p->qt);
                    defaults.push_back(p->init);
                }
                c->args = callArguments(c, params, defaults, f->name + "()");
                c->qt = f->resultType;
                c->lvalue = f->resultType.isRef;
                c->qt.isRef = false;
                if (fn_) fn_->calls.push_back(f);
                if (f->name == "prepare" || f->name == "process") {
                    if (self_ && self_->base != PluginKind::None && f->owner == self_)
                        error(c->loc, "script.call.hook",
                              f->name + "() is called by the runtime, not by the script",
                              f->name == "process"
                                  ? "move the shared work into a member function both can call"
                                  : "");
                }
                return c;
            }
            case RefKind::BuiltinMethod: {
                // A plugin base method: sampleRate() and friends.
                for (const auto& bm : b_.pluginBase()->methods)
                    if (bm.name == calleeName) {
                        c->builtinMethod = &bm;
                        c->implicitThis = true;
                        if (!c->args.empty())
                            error(c->loc, "script.call.arguments",
                                  calleeName + "() takes no arguments", "");
                        c->qt = bm.result;
                        return c;
                    }
                return c;
            }
            case RefKind::Builtin: {
                if (n->builtins.empty()) {
                    error(c->loc, "script.call.not_function",
                          "'" + calleeName + "' is a value, not a function", "");
                    return c;
                }
                // Overload resolution among the runtime's functions.
                const BuiltinFunction* best = nullptr;
                std::vector<ConversionRank> bestRanks;
                std::vector<const BuiltinFunction*> tied;
                std::vector<const BuiltinFunction*> viable;
                for (const BuiltinFunction* f : n->builtins) {
                    if (f->params.size() != c->args.size()) continue;
                    std::vector<ConversionRank> ranks;
                    bool ok = true;
                    for (size_t i = 0; i < c->args.size(); ++i) {
                        ranks.push_back(rank(c->args[i], f->params[i]));
                        ok = ok && ranks.back().rank != ConversionRank::NotViable;
                    }
                    if (!ok) continue;
                    viable.push_back(f);
                    if (!best || better(ranks, bestRanks)) {
                        best = f;
                        bestRanks = ranks;
                    }
                }
                // Ambiguity: another viable candidate the best does not beat.
                for (const BuiltinFunction* f : viable) {
                    if (f == best) continue;
                    std::vector<ConversionRank> ranks;
                    for (size_t i = 0; i < c->args.size(); ++i)
                        ranks.push_back(rank(c->args[i], f->params[i]));
                    if (!better(bestRanks, ranks)) tied.push_back(f);
                }
                auto signatureOf = [&](const BuiltinFunction* f) {
                    std::string s = f->result.type->name + " " + f->name + "(";
                    for (size_t i = 0; i < f->params.size(); ++i)
                        s += (i ? ", " : "") + typeName(f->params[i]);
                    return s + ")";
                };
                std::string argTypes;
                for (size_t i = 0; i < c->args.size(); ++i)
                    argTypes += (i ? ", " : "") +
                                (c->args[i]->type() ? c->args[i]->type()->name : std::string("?"));
                if (!best) {
                    std::string cands;
                    for (const BuiltinFunction* f : n->builtins)
                        cands += (cands.empty() ? "" : "; ") + signatureOf(f);
                    error(c->loc, "script.call.no_match",
                          "no " + calleeName + " takes (" + argTypes + ")",
                          "the runtime has: " + cands);
                    return c;
                }
                if (!tied.empty()) {
                    std::string cands = signatureOf(best);
                    for (const BuiltinFunction* f : tied) cands += " and " + signatureOf(f);
                    std::string fix =
                        calleeName == "min" || calleeName == "max" || calleeName == "clamp"
                            ? "give every argument the same type: 0.5 for doubles, double(i) for "
                              "an int"
                            : "convert the arguments so one matches exactly: 1.0 rather than 1";
                    error(
                        c->loc, "script.call.ambiguous",
                        calleeName + "(" + argTypes + ") is ambiguous in C++: it could be " + cands,
                        fix);
                    return c;
                }
                c->builtin = best;
                n->builtin = best;
                c->args = callArguments(c, best->params, {}, calleeName + "()");
                c->qt = best->result;
                // Constant folding of a few pure intrinsics keeps size
                // expressions constant: max(1, kVoices).
                return c;
            }
            case RefKind::Type: {
                error(c->loc, "script.unsupported.constructor_call",
                      "'" + calleeName + "(...)' would construct " + calleeName +
                          "; the subset has no constructors",
                      "declare a variable and set its members, or use braces: " + calleeName +
                          "{...}");
                return c;
            }
            case RefKind::Local:
            case RefKind::Field:
            case RefKind::Global:
            case RefKind::StaticConst:
                error(c->loc, "script.call.not_function",
                      "'" + calleeName + "' is a value, not a function", "");
                return c;
            default:
                return c;
        }
    }
    error(c->loc, "script.call.callee", "this cannot be called", "");
    return c;
}

// --- operators ----------------------------------------------------------------------

Expr* Sema::checkUnary(UnaryExpr* u) {
    u->operand = check(u->operand);
    const Type* t = u->operand->type();
    if (!t) return u;
    const bool isParam = t->kind == TypeKind::Object && t->builtin == b_.param();
    if (u->op == "++" || u->op == "--") {
        if (!requireModifiable(u->operand, u->loc, "'" + u->op + "'")) return u;
        if (!t->isArithmetic() || t->kind == TypeKind::Bool) {
            error(u->loc, "script.type.operator",
                  "'" + u->op + "' needs an int, float or double, not " + t->name, "");
            return u;
        }
        u->qt = QualType{t, false, false};
        u->lvalue = !u->postfix;
        return u;
    }
    if (u->op == "!") {
        u->operand = condition(u->operand);
        u->qt = QualType{b_.boolType(), false, false};
        if (u->operand->constant) {
            u->constant = true;
            u->constInt = u->operand->constInt == 0;
            u->constValue = static_cast<double>(u->constInt);
        }
        return u;
    }
    if (!(t->isArithmetic() || isParam)) {
        error(u->loc, "script.type.operator",
              "'" + u->op + "' needs a number, not " + article(t->name), "");
        return u;
    }
    const Type* operandType = isParam ? b_.doubleType() : promoted(t);
    if (u->op == "~" && operandType->kind != TypeKind::Int) {
        error(u->loc, "script.type.operator", "'~' needs an int, not " + operandType->name, "");
        return u;
    }
    u->operand = convert(u->operand, QualType{operandType, false, false}, u->loc,
                         "the operand of '" + u->op + "'");
    u->qt = QualType{operandType, false, false};
    if (u->operand->constant) {
        u->constant = true;
        if (operandType->kind == TypeKind::Int) {
            const auto v = static_cast<std::uint32_t>(u->operand->constInt);
            const std::uint32_t r = u->op == "-" ? 0u - v : u->op == "~" ? ~v : v;
            u->constInt = static_cast<std::int32_t>(r);
            u->constValue = static_cast<double>(u->constInt);
        } else if (operandType->kind == TypeKind::Float) {
            const float v = static_cast<float>(u->operand->constValue);
            u->constValue = static_cast<double>(u->op == "-" ? -v : v);
        } else {
            u->constValue = u->op == "-" ? -u->operand->constValue : u->operand->constValue;
        }
    }
    return u;
}

namespace {
// C++'s int arithmetic on two's complement (exports build with -fwrapv).
std::int64_t intOp(const std::string& op, std::int64_t a, std::int64_t b) {
    const auto x = static_cast<std::uint32_t>(a), y = static_cast<std::uint32_t>(b);
    std::uint32_t r = 0;
    if (op == "+")
        r = x + y;
    else if (op == "-")
        r = x - y;
    else if (op == "*")
        r = x * y;
    else if (op == "/")
        return b ? static_cast<std::int32_t>(a) / static_cast<std::int32_t>(b) : 0;
    else if (op == "%")
        return b ? static_cast<std::int32_t>(a) % static_cast<std::int32_t>(b) : 0;
    else if (op == "&")
        r = x & y;
    else if (op == "|")
        r = x | y;
    else if (op == "^")
        r = x ^ y;
    else if (op == "<<")
        r = x << (y & 31u);
    else if (op == ">>")
        return static_cast<std::int32_t>(a) >> (y & 31u);
    return static_cast<std::int32_t>(r);
}
}  // namespace

Expr* Sema::checkBinary(BinaryExpr* bin) {
    const std::string& op = bin->op;
    if (op == ",") {
        bin->left = check(bin->left);
        bin->right = check(bin->right);
        bin->qt = bin->right->qt;
        bin->lvalue = bin->right->lvalue;
        return bin;
    }
    if (op == "&&" || op == "||") {
        bin->left = condition(bin->left);
        bin->right = condition(bin->right);
        bin->qt = QualType{b_.boolType(), false, false};
        if (bin->left->constant && bin->right->constant) {
            bin->constant = true;
            bin->constInt = op == "&&" ? (bin->left->constInt && bin->right->constInt)
                                       : (bin->left->constInt || bin->right->constInt);
            bin->constValue = static_cast<double>(bin->constInt);
        }
        return bin;
    }
    bin->left = check(bin->left);
    bin->right = check(bin->right);
    const Type* lt = bin->left->type();
    const Type* rt = bin->right->type();
    if (!lt || !rt) return bin;
    auto numeric = [&](const Type* t) {
        return t->isArithmetic() || (t->kind == TypeKind::Object && t->builtin == b_.param());
    };
    auto asArith = [&](const Type* t) { return t->kind == TypeKind::Object ? b_.doubleType() : t; };
    const bool comparison =
        op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=";

    // Two values of one scoped enum compare.
    if (lt->kind == TypeKind::Enum || rt->kind == TypeKind::Enum) {
        if (comparison && lt == rt) {
            bin->operandType = lt;
            bin->qt = QualType{b_.boolType(), false, false};
            return bin;
        }
        error(bin->loc, "script.type.enum",
              "'" + op + "' between " + lt->name + " and " + rt->name +
                  ": a scoped enum only compares with its own values",
              "compare with a value of the enum, or convert explicitly with static_cast<int>(...)");
        return bin;
    }
    if (!numeric(lt) || !numeric(rt)) {
        const Type* bad = numeric(lt) ? rt : lt;
        error(bin->loc, "script.type.operator",
              "'" + op + "' needs numbers, not " + article(bad->name), "");
        return bin;
    }
    const bool integralOnly =
        op == "%" || op == "<<" || op == ">>" || op == "&" || op == "|" || op == "^";
    const Type* l = promoted(asArith(lt));
    const Type* r = promoted(asArith(rt));
    if (integralOnly && (l->isFloating() || r->isFloating())) {
        error(bin->loc, "script.type.operator",
              "'" + op + "' needs ints, not " + (l->isFloating() ? l->name : r->name),
              op == "%" ? "for the remainder of doubles use fmod(x, y)" : "");
        return bin;
    }
    const Type* common = (op == "<<" || op == ">>") ? l : arithmeticResult(l, r);
    bin->operandType = common;
    if (op == "<<" || op == ">>") {
        bin->left =
            convert(bin->left, QualType{l, false, false}, bin->loc, "the left of '" + op + "'");
        bin->right =
            convert(bin->right, QualType{r, false, false}, bin->loc, "the right of '" + op + "'");
        if (!bin->right->constant || bin->right->constInt < 0 || bin->right->constInt > 31)
            error(bin->right->loc, "script.ub.shift",
                  "a shift amount must be a constant from 0 to 31 (C++ leaves other amounts "
                  "undefined)",
                  "shift by a literal, or multiply/divide by a power of two");
    } else {
        bin->left = convert(bin->left, QualType{common, false, false}, bin->loc,
                            "the left of '" + op + "'");
        bin->right = convert(bin->right, QualType{common, false, false}, bin->loc,
                             "the right of '" + op + "'");
    }
    if ((op == "/" || op == "%") && common->kind == TypeKind::Int) {
        if (!bin->right->constant || bin->right->constInt <= 0)
            error(bin->right->loc, "script.ub.division",
                  std::string("integer ") + (op == "/" ? "division" : "remainder") +
                      " needs a positive constant divisor (C++ leaves a zero divisor undefined)",
                  op == "/"
                      ? "use idiv(a, b), which gives 0 for b == 0, or divide doubles"
                      : "use imod(a, b), or wrap(i, n) for an index that must stay in 0..n-1");
    }
    bin->qt = QualType{comparison ? b_.boolType() : common, false, false};
    // Constant folding, as C++ evaluates the same operation.
    if (bin->left->constant && bin->right->constant) {
        bin->constant = true;
        if (common->kind == TypeKind::Int) {
            const std::int64_t a = bin->left->constInt, b = bin->right->constInt;
            if (comparison) {
                bin->constInt = op == "<"    ? a < b
                                : op == "<=" ? a <= b
                                : op == ">"  ? a > b
                                : op == ">=" ? a >= b
                                : op == "==" ? a == b
                                             : a != b;
            } else {
                bin->constInt = intOp(op, a, b);
            }
            bin->constValue = static_cast<double>(bin->constInt);
        } else if (common->kind == TypeKind::Float) {
            const float a = static_cast<float>(bin->left->constValue),
                        b = static_cast<float>(bin->right->constValue);
            if (comparison) {
                bin->constInt = op == "<"    ? a < b
                                : op == "<=" ? a <= b
                                : op == ">"  ? a > b
                                : op == ">=" ? a >= b
                                : op == "==" ? a == b
                                             : a != b;
                bin->constValue = static_cast<double>(bin->constInt);
            } else {
                const float v = op == "+" ? a + b : op == "-" ? a - b : op == "*" ? a * b : a / b;
                bin->constValue = static_cast<double>(v);
            }
        } else {
            const double a = bin->left->constValue, b = bin->right->constValue;
            if (comparison) {
                bin->constInt = op == "<"    ? a < b
                                : op == "<=" ? a <= b
                                : op == ">"  ? a > b
                                : op == ">=" ? a >= b
                                : op == "==" ? a == b
                                             : a != b;
                bin->constValue = static_cast<double>(bin->constInt);
            } else {
                bin->constValue = op == "+" ? a + b : op == "-" ? a - b : op == "*" ? a * b : a / b;
            }
        }
    }
    return bin;
}

Expr* Sema::checkAssign(AssignExpr* a) {
    a->target = check(a->target);
    if (a->value->kind == ExprKind::InitList)
        a->value->qt = QualType{b_.initListType(), false, false};
    else
        a->value = check(a->value);
    const Type* t = a->target->type();
    if (!t || !a->value->type()) return a;
    if (!requireModifiable(a->target, a->loc, "assignment")) return a;
    a->qt = QualType{t, false, false};
    a->lvalue = true;
    if (a->op == "=") {
        a->value = convert(a->value, QualType{t, false, false}, a->loc, "the assigned value");
        return a;
    }
    // Compound: target op value, in the usual arithmetic conversions' type,
    // then converted back to the target's type.
    const std::string op = a->op.substr(0, a->op.size() - 1);
    const Type* vt = a->value->type();
    const bool vParam = vt->kind == TypeKind::Object && vt->builtin == b_.param();
    if (!t->isArithmetic() || !(vt->isArithmetic() || vParam)) {
        error(a->loc, "script.type.operator", "'" + a->op + "' needs numbers", "");
        return a;
    }
    const Type* l = promoted(t);
    const Type* r = promoted(vParam ? b_.doubleType() : vt);
    const bool integralOnly =
        op == "%" || op == "<<" || op == ">>" || op == "&" || op == "|" || op == "^";
    if (integralOnly && (l->isFloating() || r->isFloating())) {
        error(a->loc, "script.type.operator", "'" + a->op + "' needs ints", "");
        return a;
    }
    const Type* common = (op == "<<" || op == ">>") ? l : arithmeticResult(l, r);
    if (common->isFloating() && t->kind == TypeKind::Int) {
        error(a->loc, "script.type.float_to_int",
              "'" + a->op + "' computes in " + common->name + " and stores the result in an int",
              kToIntHint);
        return a;
    }
    if (t->kind == TypeKind::Bool) {
        error(a->loc, "script.type.operator", "'" + a->op + "' on a bool",
              "assign the bool a condition instead");
        return a;
    }
    a->operandType = common;
    a->value = convert(a->value, QualType{(op == "<<" || op == ">>") ? r : common, false, false},
                       a->loc, "the right of '" + a->op + "'");
    if ((op == "/" || op == "%") && common->kind == TypeKind::Int &&
        (!a->value->constant || a->value->constInt <= 0))
        error(a->value->loc, "script.ub.division",
              "integer division needs a positive constant divisor", "use idiv(a, b) or imod(a, b)");
    if ((op == "<<" || op == ">>") &&
        (!a->value->constant || a->value->constInt < 0 || a->value->constInt > 31))
        error(a->value->loc, "script.ub.shift", "a shift amount must be a constant from 0 to 31",
              "");
    return a;
}

Expr* Sema::checkConditional(ConditionalExpr* c) {
    c->cond = condition(c->cond);
    c->then = check(c->then);
    c->otherwise = check(c->otherwise);
    const Type* a = c->then->type();
    const Type* b = c->otherwise->type();
    if (!a || !b) return c;
    if (a == b) {
        c->qt = QualType{a, c->then->qt.isConst || c->otherwise->qt.isConst, false};
        c->lvalue = c->then->lvalue && c->otherwise->lvalue;
        if (!a->isScalar() && !c->lvalue && a->kind != TypeKind::Object)
            error(c->loc, "script.type.conditional",
                  "both branches of ?: must be variables to choose between " + a->name, "");
        if (c->cond->constant && c->then->constant && c->otherwise->constant) {
            c->constant = true;
            c->constInt = c->cond->constInt ? c->then->constInt : c->otherwise->constInt;
            c->constValue = c->cond->constInt ? c->then->constValue : c->otherwise->constValue;
        }
        if (a->kind == TypeKind::Object && a->builtin == b_.param()) c->lvalue = true;
        return c;
    }
    auto arith = [&](const Type* t) {
        return t->isArithmetic()                                           ? t
               : (t->kind == TypeKind::Object && t->builtin == b_.param()) ? b_.doubleType()
                                                                           : nullptr;
    };
    const Type* x = arith(a);
    const Type* y = arith(b);
    if (!x || !y) {
        error(c->loc, "script.type.conditional",
              "the branches of ?: have different types: " + a->name + " and " + b->name, "");
        return c;
    }
    const Type* common = arithmeticResult(promoted(x), promoted(y));
    if (x->kind == TypeKind::Bool && y->kind == TypeKind::Bool) common = b_.boolType();
    c->then = convert(c->then, QualType{common, false, false}, c->loc, "a branch of ?:");
    c->otherwise = convert(c->otherwise, QualType{common, false, false}, c->loc, "a branch of ?:");
    c->qt = QualType{common, false, false};
    if (c->cond->constant && c->then->constant && c->otherwise->constant) {
        c->constant = true;
        c->constInt = c->cond->constInt ? c->then->constInt : c->otherwise->constInt;
        c->constValue = c->cond->constInt ? c->then->constValue : c->otherwise->constValue;
    }
    return c;
}

Expr* Sema::checkCast(CastExpr* c) {
    if (c->cast == CastKind::Implicit) return c;
    const Type* t = resolve(c->target, self_);
    if (!t) return c;
    if (c->operand && c->operand->kind == ExprKind::InitList) {
        // T{...}: list-initialization of a value.
        c->operand->qt = QualType{b_.initListType(), false, false};
        Expr* v = convert(c->operand, QualType{t, false, false}, c->loc, t->name + "{...}");
        c->qt = QualType{t, false, false};
        c->operand = v;
        c->constant = v->constant;
        c->constInt = v->constInt;
        c->constValue = v->constValue;
        if (!t->isScalar() && !declarable(t))
            error(c->loc, "script.type.temporary", t->name + " cannot be made as a temporary", "");
        return c;
    }
    c->operand = check(c->operand);
    const Type* f = c->operand->type();
    if (!f) return c;
    c->qt = QualType{t, false, false};
    if (!t->isScalar()) {
        error(c->loc, "script.type.cast",
              "a cast converts between numbers and enums, not to " + t->name, "");
        return c;
    }
    const bool fromParam = f->kind == TypeKind::Object && f->builtin == b_.param();
    if (fromParam) f = b_.doubleType();
    if (!f->isScalar()) {
        error(c->loc, "script.type.cast",
              "cannot convert " + article(c->operand->type()->name) + " to " + t->name, "");
        return c;
    }
    if (f->isFloating() && (t->kind == TypeKind::Int || t->kind == TypeKind::Enum)) {
        error(c->loc, "script.type.float_to_int", "converting " + f->name + " to " + t->name,
              kToIntHint);
        return c;
    }
    if (t->kind == TypeKind::Enum && f->kind != TypeKind::Int && f != t) {
        error(c->loc, "script.type.cast",
              "an enum is made from an int: static_cast<" + t->name + ">(i)", "");
        return c;
    }
    if (f->kind == TypeKind::Enum && t != f && t->kind != TypeKind::Int) {
        error(c->loc, "script.type.cast", "an enum converts to int: static_cast<int>(value)", "");
        return c;
    }
    if (fromParam) c->operand = implicitCast(c->operand, b_.doubleType(), true);
    if (c->operand->constant) {
        c->constant = true;
        if (t->kind == TypeKind::Double)
            c->constValue = f->isFloating() ? c->operand->constValue
                                            : static_cast<double>(c->operand->constInt);
        else if (t->kind == TypeKind::Float)
            c->constValue =
                static_cast<double>(f->isFloating() ? static_cast<float>(c->operand->constValue)
                                                    : static_cast<float>(c->operand->constInt));
        else if (t->kind == TypeKind::Bool)
            c->constInt =
                f->isFloating() ? c->operand->constValue != 0.0 : c->operand->constInt != 0;
        else
            c->constInt = c->operand->constInt;
        if (t->isIntegral() || t->kind == TypeKind::Enum)
            c->constValue = static_cast<double>(c->constInt);
    }
    return c;
}

Expr* Sema::check(Expr* e) {
    if (!e) return e;
    if (e->type() && e->kind != ExprKind::InitList) return e;  // already checked
    switch (e->kind) {
        case ExprKind::IntLit: {
            auto* l = static_cast<IntLitExpr*>(e);
            l->qt = QualType{b_.intType(), true, false};
            l->constant = true;
            l->constInt = l->value;
            l->constValue = static_cast<double>(l->value);
            return l;
        }
        case ExprKind::FloatLit: {
            auto* l = static_cast<FloatLitExpr*>(e);
            l->qt = QualType{l->isFloat ? b_.floatType() : b_.doubleType(), true, false};
            l->constant = true;
            l->constValue = l->value;
            return l;
        }
        case ExprKind::BoolLit: {
            auto* l = static_cast<BoolLitExpr*>(e);
            l->qt = QualType{b_.boolType(), true, false};
            l->constant = true;
            l->constInt = l->value;
            l->constValue = l->value ? 1.0 : 0.0;
            return l;
        }
        case ExprKind::StringLit:
            e->qt = QualType{b_.stringType(), true, false};
            return e;
        case ExprKind::NullLit:
            e->qt = QualType{b_.nullType(), true, false};
            return e;
        case ExprKind::Name: {
            auto* n = static_cast<NameExpr*>(e);
            Expr* r = checkName(n);
            if (r->kind == ExprKind::Name) {
                auto* rn = static_cast<NameExpr*>(r);
                if (!rn->qt.type &&
                    (rn->ref == RefKind::Function || rn->ref == RefKind::Method ||
                     rn->ref == RefKind::Builtin || rn->ref == RefKind::BuiltinMethod)) {
                    // Only meaningful as a callee; checkCall reads it.
                    rn->qt = QualType{b_.voidType(), true, false};
                    if (!inCallee_)
                        error(e->loc, "script.call.missing_parens",
                              "'" + rn->path.back() + "' is a function; call it with ()",
                              rn->path.back() == "sampleRate" ? "sampleRate()" : "");
                } else if (!rn->qt.type && rn->ref == RefKind::Type) {
                    rn->qt = QualType{b_.voidType(), true, false};
                    error(e->loc, "script.name.type",
                          "'" + rn->path.back() + "' is a type, not a value", "");
                }
            }
            return r;
        }
        case ExprKind::Member: {
            Expr* r = checkMember(static_cast<MemberExpr*>(e));
            if (r->kind == ExprKind::Member) {
                auto* m = static_cast<MemberExpr*>(r);
                if (!m->qt.type &&
                    (m->ref == RefKind::Method || m->ref == RefKind::BuiltinMethod)) {
                    m->qt = QualType{b_.voidType(), true, false};
                    if (!inCallee_)
                        error(e->loc, "script.call.missing_parens",
                              "'" + m->member + "' is a member function; call it with ()", "");
                }
            } else if (r->kind == ExprKind::Name && !r->qt.type) {
                r->qt = QualType{b_.voidType(), true, false};
            }
            return r;
        }
        case ExprKind::Index:
            return checkIndex(static_cast<IndexExpr*>(e));
        case ExprKind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if (c->callee->kind == ExprKind::Name || c->callee->kind == ExprKind::Member) {
                inCallee_ = true;
                c->callee = check(c->callee);
                inCallee_ = false;
            }
            Expr* r = checkCall(c);
            if (!r->qt.type) r->qt = QualType{b_.voidType(), false, false};
            return r;
        }
        case ExprKind::Unary:
            return checkUnary(static_cast<UnaryExpr*>(e));
        case ExprKind::Binary:
            return checkBinary(static_cast<BinaryExpr*>(e));
        case ExprKind::Assign:
            return checkAssign(static_cast<AssignExpr*>(e));
        case ExprKind::Conditional:
            return checkConditional(static_cast<ConditionalExpr*>(e));
        case ExprKind::Cast:
            return checkCast(static_cast<CastExpr*>(e));
        case ExprKind::InitList:
            e->qt = QualType{b_.initListType(), false, false};
            return e;
        case ExprKind::This:
            error(e->loc, "script.unsupported.this", "'this' is only usable as this->member", "");
            e->qt = QualType{b_.voidType(), false, false};
            return e;
        case ExprKind::TypeName:
            error(e->loc, "script.name.type", "a type is not a value", "");
            e->qt = QualType{b_.voidType(), false, false};
            return e;
    }
    return e;
}

// --- constant evaluation ---------------------------------------------------------------

bool Sema::constantInt(Expr* e, std::int64_t& out) {
    Expr* c = check(e);
    if (!c || !c->constant || !c->type() ||
        !(c->type()->isIntegral() || c->type()->kind == TypeKind::Enum))
        return false;
    out = c->constInt;
    return true;
}

namespace {
Constant num(double v) {
    Constant c;
    c.kind = Constant::Kind::Double;
    c.doubleValue = v;
    return c;
}
}  // namespace

// Evaluates what a declaration's value may be: numbers, text, lists, and the
// parameter factories and their modifiers.
std::optional<Constant> Sema::evaluate(Expr* e, StructDecl* scope) {
    if (!e) return std::nullopt;
    StructDecl* savedSelf = self_;
    if (scope) self_ = scope;
    struct Restore {
        Sema* s;
        StructDecl* saved;
        ~Restore() { s->self_ = saved; }
    } restore{this, savedSelf};

    if (e->kind == ExprKind::StringLit) {
        Constant c;
        c.kind = Constant::Kind::String;
        c.stringValue = static_cast<StringLitExpr*>(e)->value;
        return c;
    }
    if (e->kind == ExprKind::InitList) {
        Constant c;
        c.kind = Constant::Kind::List;
        for (auto& item : static_cast<InitListExpr*>(e)->items) {
            auto v = evaluate(item.value, scope);
            if (!v) return std::nullopt;
            c.list.push_back(*v);
            c.designators.push_back(item.designator);
        }
        return c;
    }
    if (e->kind == ExprKind::Call) {
        auto* call = static_cast<CallExpr*>(e);
        // A factory: decibels("Gain", -24, 24, 0).
        if (call->callee->kind == ExprKind::Name &&
            static_cast<NameExpr*>(call->callee)->path.size() == 1) {
            const std::string& name = static_cast<NameExpr*>(call->callee)->path[0];
            static const std::map<std::string, std::pair<ParamModel::Unit, int>> factories = {
                {"continuous", {ParamModel::Unit::None, 4}},
                {"decibels", {ParamModel::Unit::Decibels, 4}},
                {"frequency", {ParamModel::Unit::Hertz, 4}},
                {"milliseconds", {ParamModel::Unit::Milliseconds, 4}},
                {"percent", {ParamModel::Unit::Percent, 2}},
                {"ratio", {ParamModel::Unit::Ratio, 4}},
                {"semitones", {ParamModel::Unit::Semitones, 4}},
                {"cents", {ParamModel::Unit::Cents, 4}},
                {"octaves", {ParamModel::Unit::Octaves, 4}},
                {"degrees", {ParamModel::Unit::Degrees, 4}},
                {"integer", {ParamModel::Unit::None, 4}},
                {"choice", {ParamModel::Unit::None, 3}},
                {"toggle", {ParamModel::Unit::None, 2}}};
            auto it = factories.find(name);
            if (it != factories.end()) {
                Constant c;
                c.kind = Constant::Kind::Spec;
                ParamModel& p = c.spec;
                std::vector<Constant> args;
                for (Expr* a : call->args) {
                    auto v = evaluate(a, scope);
                    if (!v) return std::nullopt;
                    args.push_back(*v);
                }
                auto need = [&](size_t i, Constant::Kind kind, const char* what) -> bool {
                    if (i >= args.size()) return false;
                    if (kind == Constant::Kind::Double ? !args[i].numeric()
                                                       : args[i].kind != kind) {
                        error(call->args[i]->loc, "script.param.argument",
                              name + "(): the " + what + " must be " +
                                  (kind == Constant::Kind::String ? "text in quotes"
                                   : kind == Constant::Kind::List ? "a list of names in braces"
                                                                  : "a constant number"),
                              "");
                        return false;
                    }
                    return true;
                };
                const size_t minArgs = name == "percent" || name == "toggle" ? 2
                                       : name == "choice"                    ? 3
                                                                             : 4;
                const size_t maxArgs = name == "percent" ? 4 : name == "continuous" ? 5 : minArgs;
                if (args.size() < minArgs || args.size() > maxArgs) {
                    std::string example = name + "(\"Name\", min, max, default)";
                    if (name == "percent")
                        example =
                            "percent(\"Mix\", 100)  (the default in percent; optional min and max)";
                    if (name == "toggle") example = "toggle(\"Enabled\", true)";
                    if (name == "choice") example = "choice(\"Mode\", {\"Clean\", \"Warm\"}, 0)";
                    error(call->loc, "script.param.arguments",
                          name + "() takes " + std::to_string(minArgs) +
                              (maxArgs > minArgs ? " to " + std::to_string(maxArgs) : "") +
                              " arguments",
                          example);
                    return std::nullopt;
                }
                if (!need(0, Constant::Kind::String, "name")) return std::nullopt;
                p.name = args[0].stringValue;
                p.unit = it->second.first;
                if (name == "toggle") {
                    if (!need(1, Constant::Kind::Double, "default")) return std::nullopt;
                    p.kind = ParamModel::Kind::Toggle;
                    p.min = 0;
                    p.max = 1;
                    p.def = args[1].asDouble() != 0.0 ? 1.0 : 0.0;
                } else if (name == "choice") {
                    if (!need(1, Constant::Kind::List, "options") ||
                        !need(2, Constant::Kind::Double, "default"))
                        return std::nullopt;
                    p.kind = ParamModel::Kind::Choice;
                    for (const Constant& o : args[1].list) {
                        if (o.kind != Constant::Kind::String) {
                            error(call->args[1]->loc, "script.param.argument",
                                  "choice(): each option is text in quotes", "");
                            return std::nullopt;
                        }
                        p.choices.push_back(o.stringValue);
                    }
                    p.min = 0;
                    p.max = static_cast<double>(p.choices.size()) - 1.0;
                    p.def = args[2].asDouble();
                } else if (name == "percent") {
                    for (size_t i = 1; i < args.size(); ++i)
                        if (!need(i, Constant::Kind::Double,
                                  i == 1   ? "default"
                                  : i == 2 ? "minimum"
                                           : "maximum"))
                            return std::nullopt;
                    p.def = args[1].asDouble();
                    p.min = args.size() > 2 ? args[2].asDouble() : 0.0;
                    p.max = args.size() > 3 ? args[3].asDouble() : 100.0;
                } else {
                    for (size_t i = 1; i <= 3; ++i)
                        if (!need(i, Constant::Kind::Double,
                                  i == 1   ? "minimum"
                                  : i == 2 ? "maximum"
                                           : "default"))
                            return std::nullopt;
                    p.min = args[1].asDouble();
                    p.max = args[2].asDouble();
                    p.def = args[3].asDouble();
                    if (name == "integer") p.kind = ParamModel::Kind::Integer;
                    if (name == "frequency" || name == "ratio") p.curve = ParamModel::Curve::Log;
                    if (name == "milliseconds") {
                        if (p.min > 0)
                            p.curve = ParamModel::Curve::Log;
                        else {
                            p.curve = ParamModel::Curve::Pow;
                            p.curveK = 3.0;
                        }
                    }
                    if (name == "continuous" && args.size() == 5) {
                        error(call->args[4]->loc, "script.param.argument",
                              "continuous()'s unit: use the factory for it (decibels, frequency, "
                              "...)",
                              "");
                        return std::nullopt;
                    }
                }
                return c;
            }
        }
        // A modifier on a spec: spec.smoothed(20).
        if (call->callee->kind == ExprKind::Member) {
            auto* m = static_cast<MemberExpr*>(call->callee);
            auto base = evaluate(m->object, scope);
            if (!base) return std::nullopt;
            if (base->kind != Constant::Kind::Spec) {
                error(call->loc, "script.param.modifier",
                      "'." + m->member + "()' applies to a parameter's description", "");
                return std::nullopt;
            }
            Constant c = *base;
            ParamModel& p = c.spec;
            std::vector<Constant> args;
            for (Expr* a : call->args) {
                auto v = evaluate(a, scope);
                if (!v) return std::nullopt;
                args.push_back(*v);
            }
            auto text = [&](size_t i) -> std::optional<std::string> {
                if (i < args.size() && args[i].kind == Constant::Kind::String)
                    return args[i].stringValue;
                error(call->loc, "script.param.modifier",
                      "." + m->member + "() takes text in quotes", "");
                return std::nullopt;
            };
            auto number = [&](size_t i) -> std::optional<double> {
                if (i < args.size() && args[i].numeric()) return args[i].asDouble();
                error(call->loc, "script.param.modifier",
                      "." + m->member + "() takes a constant number", "");
                return std::nullopt;
            };
            const std::string& mod = m->member;
            if (mod == "in") {
                auto t = text(0);
                if (!t) return std::nullopt;
                p.group = *t;
            } else if (mod == "help") {
                auto t = text(0);
                if (!t) return std::nullopt;
                p.description = *t;
            } else if (mod == "withKey") {
                auto t = text(0);
                if (!t) return std::nullopt;
                p.key = *t;
            } else if (mod == "withId") {
                auto v = number(0);
                if (!v) return std::nullopt;
                if (*v < 1 || *v > 4294967294.0 || *v != std::floor(*v)) {
                    error(call->loc, "script.param.id",
                          "a parameter id is a whole number from 1 to 4294967294", "");
                    return std::nullopt;
                }
                p.id = static_cast<std::uint32_t>(*v);
            } else if (mod == "smoothed") {
                auto v = number(0);
                if (!v) return std::nullopt;
                p.smoothingMs = *v;
            } else if (mod == "modulated") {
                p.modulatable = true;
            } else if (mod == "offAtMin") {
                p.offAtMinimum = true;
            } else if (mod == "meter") {
                p.output = true;
                p.automatable = false;
            } else if (mod == "notAutomatable") {
                p.automatable = false;
            } else if (mod == "role") {
                if (args.size() != 1 || args[0].kind != Constant::Kind::List) {
                    error(call->loc, "script.param.modifier",
                          ".role() takes a list: .role({\"level\"})", "");
                    return std::nullopt;
                }
                for (const Constant& r : args[0].list) {
                    if (r.kind != Constant::Kind::String) {
                        error(call->loc, "script.param.modifier", ".role(): each role is text", "");
                        return std::nullopt;
                    }
                    p.roles.push_back(r.stringValue);
                }
            } else if (mod == "curved") {
                if (args.empty() || args[0].kind != Constant::Kind::Enum) {
                    error(call->loc, "script.param.modifier",
                          ".curved() takes Curve::Linear, Curve::Log or Curve::Pow", "");
                    return std::nullopt;
                }
                p.curve = static_cast<ParamModel::Curve>(args[0].intValue);
                p.curveK = 1.0;
                if (args.size() > 1) {
                    auto k = number(1);
                    if (!k) return std::nullopt;
                    p.curveK = *k;
                }
            } else {
                error(m->loc, "script.param.modifier",
                      "a parameter has no modifier '." + mod + "()'",
                      "the modifiers are .in(group), .help(text), .smoothed(ms), .modulated(), "
                      ".offAtMin(), "
                      ".meter(), .notAutomatable(), .role({...}), .curved(Curve::..., k), "
                      ".withKey(key), .withId(n)");
                return std::nullopt;
            }
            return c;
        }
    }
    // Curve::Log and the like, inside parameter declarations.
    if (e->kind == ExprKind::Name) {
        auto* n = static_cast<NameExpr*>(e);
        if (n->path.size() == 2 && n->path[0] == "Curve") {
            Constant c;
            c.kind = Constant::Kind::Enum;
            if (n->path[1] == "Linear")
                c.intValue = 0;
            else if (n->path[1] == "Log")
                c.intValue = 1;
            else if (n->path[1] == "Pow")
                c.intValue = 2;
            else {
                error(n->loc, "script.param.curve", "Curve has Linear, Log and Pow", "");
                return std::nullopt;
            }
            return c;
        }
    }
    // Numbers and enumerators, through the expression checker.
    Expr* checked = check(e);
    if (!checked->constant) {
        error(e->loc, "script.constant.required", "this must be a constant here",
              "use literals, constexpr constants and arithmetic on them");
        return std::nullopt;
    }
    Constant c;
    const Type* t = checked->type();
    if (t->kind == TypeKind::Enum) {
        c.kind = Constant::Kind::Enum;
        c.intValue = checked->constInt;
        c.enumType = t;
    } else if (t->kind == TypeKind::Bool) {
        c.kind = Constant::Kind::Bool;
        c.boolValue = checked->constInt != 0;
    } else if (t->kind == TypeKind::Int) {
        c.kind = Constant::Kind::Int;
        c.intValue = checked->constInt;
    } else {
        c = num(checked->constValue);
    }
    return c;
}

}  // namespace supple::scriptc
