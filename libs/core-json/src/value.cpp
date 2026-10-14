// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/json/value.h>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>
namespace supple::json {
Value::Value() = default;
Value::Value(std::nullptr_t) {}
Value::Value(bool v) : data_(v) {}
Value::Value(int v) : data_(static_cast<std::int64_t>(v)) {}
Value::Value(long v) : data_(static_cast<std::int64_t>(v)) {}
Value::Value(long long v) : data_(static_cast<std::int64_t>(v)) {}
Value::Value(unsigned v) : Value(static_cast<unsigned long long>(v)) {}
Value::Value(unsigned long v) : Value(static_cast<unsigned long long>(v)) {}
Value::Value(unsigned long long v) {
    if (v <= static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()))
        data_ = static_cast<std::int64_t>(v);
    else
        data_ = static_cast<double>(v);
}
Value::Value(double v) : data_(v) {}
Value::Value(const char* v) : data_(std::string(v)) {}
Value::Value(std::string v) : data_(std::move(v)) {}
Value::Value(std::string_view v) : data_(std::string(v)) {}
Value::Value(Array v) : data_(std::make_unique<Array>(std::move(v))) {}
Value::Value(Object v) : data_(std::make_unique<Object>(std::move(v))) {}
Value::Value(const Value& v) { *this = v; }
Value& Value::operator=(const Value& v) {
    if (this == &v) return *this;
    if (v.isArray())
        data_ = std::make_unique<Array>(v.asArray());
    else if (v.isObject())
        data_ = std::make_unique<Object>(v.asObject());
    else
        data_ = std::visit(
            [](const auto& x) -> Storage {
                using X = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<X, std::unique_ptr<Array>> ||
                              std::is_same_v<X, std::unique_ptr<Object>>)
                    return {};
                else
                    return x;
            },
            v.data_);
    return *this;
}
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;
Value Value::array() { return Array{}; }
Value Value::object() { return Object{}; }
Type Value::type() const { return static_cast<Type>(data_.index()); }
bool Value::isNull() const { return type() == Type::Null; }
bool Value::isBool() const { return type() == Type::Bool; }
bool Value::isInt() const { return type() == Type::Int; }
bool Value::isDouble() const { return type() == Type::Double; }
bool Value::isNumber() const { return isInt() || isDouble(); }
bool Value::isString() const { return type() == Type::String; }
bool Value::isArray() const { return type() == Type::Array; }
bool Value::isObject() const { return type() == Type::Object; }
bool Value::asBool() const {
    assert(isBool());
    return isBool() ? std::get<bool>(data_) : false;
}
std::int64_t Value::asInt() const {
    assert(isNumber());
    if (isInt()) return std::get<std::int64_t>(data_);
    if (isDouble()) {
        double d = std::get<double>(data_);
        if (std::isfinite(d) && std::trunc(d) == d) {
            if (d >= 0x1p63) return std::numeric_limits<std::int64_t>::max();
            if (d < -0x1p63) return std::numeric_limits<std::int64_t>::min();
            return static_cast<std::int64_t>(d);
        }
    }
    return 0;
}
double Value::asDouble() const {
    assert(isNumber());
    return isInt()      ? static_cast<double>(std::get<std::int64_t>(data_))
           : isDouble() ? std::get<double>(data_)
                        : 0.0;
}
const std::string& Value::asString() const {
    assert(isString());
    static const std::string empty;
    return isString() ? std::get<std::string>(data_) : empty;
}
const Array& Value::asArray() const {
    assert(isArray());
    static const Array empty;
    return isArray() ? *std::get<std::unique_ptr<Array>>(data_) : empty;
}
Array& Value::asArray() {
    assert(isArray());
    static Array empty;
    return isArray() ? *std::get<std::unique_ptr<Array>>(data_) : empty;
}
const Object& Value::asObject() const {
    assert(isObject());
    static const Object empty;
    return isObject() ? *std::get<std::unique_ptr<Object>>(data_) : empty;
}
Object& Value::asObject() {
    assert(isObject());
    static Object empty;
    return isObject() ? *std::get<std::unique_ptr<Object>>(data_) : empty;
}
const Value* Value::find(std::string_view key) const {
    return isObject() ? asObject().find(key) : nullptr;
}
Value& Value::operator[](std::string_view key) {
    if (isNull()) *this = object();
    assert(isObject());
    return asObject()[key];
}
const Value& Value::at(std::size_t i) const {
    assert(isArray() && i < asArray().size());
    static const Value empty;
    return isArray() && i < asArray().size() ? asArray()[i] : empty;
}
void Value::push_back(Value v) {
    if (isNull()) *this = array();
    assert(isArray());
    if (isArray()) asArray().push_back(std::move(v));
}
std::size_t Value::size() const {
    return isArray() ? asArray().size() : isObject() ? asObject().size() : 0;
}
namespace {
constexpr std::size_t kIndexMin = 16;
}
Object::Object(Object&& other) noexcept
    : members_(std::move(other.members_)), index_(std::move(other.index_)) {
    other.members_.clear();
    other.index_.clear();
}
Object& Object::operator=(const Object& other) {
    if (this != &other) {
        Object copy(other);
        *this = std::move(copy);
    }
    return *this;
}
Object& Object::operator=(Object&& other) noexcept {
    if (this != &other) {
        members_ = std::move(other.members_);
        index_ = std::move(other.index_);
        other.members_.clear();
        other.index_.clear();
    }
    return *this;
}
std::size_t Object::size() const { return members_.size(); }
bool Object::empty() const { return members_.empty(); }
void Object::rebuildIndex() {
    index_.clear();
    if (members_.size() < kIndexMin) return;
    index_.reserve(members_.size());
    for (std::size_t i = 0; i < members_.size(); ++i) index_.emplace(members_[i].key, i);
}
Value& Object::append(std::string key, Value v) {
    if (members_.size() == members_.capacity()) {
        // Grow by hand: vector's own reallocation would copy members (their
        // move constructor is not noexcept), deep-copying every value.
        std::vector<Member> grown;
        grown.reserve(std::max<std::size_t>(4, members_.size() * 2));
        for (auto& m : members_) grown.emplace_back(std::move(m));
        members_.swap(grown);
    }
    members_.emplace_back(std::move(key), std::move(v));
    if (members_.size() == kIndexMin)
        rebuildIndex();
    else if (members_.size() > kIndexMin)
        index_.emplace(members_.back().key, members_.size() - 1);
    return members_.back().value;
}
const Value* Object::find(std::string_view key) const {
    if (!index_.empty()) {
        auto it = index_.find(std::string(key));
        return it == index_.end() ? nullptr : &members_[it->second].value;
    }
    for (const auto& m : members_)
        if (m.key == key) return &m.value;
    return nullptr;
}
Value* Object::find(std::string_view key) {
    return const_cast<Value*>(std::as_const(*this).find(key));
}
bool Object::contains(std::string_view key) const { return find(key) != nullptr; }
Value& Object::set(std::string key, Value v) {
    if (auto* p = find(key)) {
        *p = std::move(v);
        return *p;
    }
    return append(std::move(key), std::move(v));
}
bool Object::erase(std::string_view key) {
    auto it = std::find_if(members_.begin(), members_.end(),
                           [key](const Member& m) { return m.key == key; });
    if (it == members_.end()) return false;
    const auto pos = static_cast<std::size_t>(it - members_.begin());
    std::vector<Member> kept;
    kept.reserve(members_.capacity());
    for (std::size_t i = 0; i < members_.size(); ++i)
        if (i != pos) kept.emplace_back(std::move(members_[i]));
    members_.swap(kept);
    if (members_.size() < kIndexMin) {
        index_.clear();
    } else {
        index_.erase(std::string(key));
        for (auto& entry : index_)
            if (entry.second > pos) --entry.second;
    }
    return true;
}
Value& Object::operator[](std::string_view key) {
    if (auto* p = find(key)) return *p;
    return append(std::string(key), {});
}
void Object::sortKeys() {
    std::vector<std::size_t> order(members_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [this](std::size_t a, std::size_t b) { return members_[a].key < members_[b].key; });
    std::vector<Member> sorted;
    sorted.reserve(members_.capacity());
    for (auto i : order) sorted.emplace_back(std::move(members_[i]));
    members_.swap(sorted);
    rebuildIndex();
}
bool operator==(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
        if (!a.isInt() && !b.isInt()) return a.asDouble() == b.asDouble();
        const auto integer = a.isInt() ? a.asInt() : b.asInt();
        const double number = a.isInt() ? b.asDouble() : a.asDouble();
        // long double has only 53 significant bits on ARM64 and Windows.
        // Check range/integrality before converting the double, so comparison
        // neither rounds the integer nor invokes an out-of-range conversion.
        return number >= -0x1p63 && number < 0x1p63 && std::trunc(number) == number &&
               integer == static_cast<std::int64_t>(number);
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Type::Null:
            return true;
        case Type::Bool:
            return a.asBool() == b.asBool();
        case Type::Int:
            return a.asInt() == b.asInt();
        case Type::Double:
            return a.asDouble() == b.asDouble();
        case Type::String:
            return a.asString() == b.asString();
        case Type::Array:
            return a.asArray() == b.asArray();
        case Type::Object:
            if (a.size() != b.size()) return false;
            for (const auto& m : a.asObject()) {
                const auto* p = b.find(m.key);
                if (!p || !(m.value == *p)) return false;
            }
            return true;
    }
    return false;
}
bool identical(const Value& a, const Value& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
        case Type::Null:
            return true;
        case Type::Bool:
            return a.asBool() == b.asBool();
        case Type::Int:
            return a.asInt() == b.asInt();
        case Type::Double:
            return std::bit_cast<std::uint64_t>(a.asDouble()) ==
                   std::bit_cast<std::uint64_t>(b.asDouble());
        case Type::String:
            return a.asString() == b.asString();
        case Type::Array:
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (!identical(a.at(i), b.at(i))) return false;
            return true;
        case Type::Object: {
            if (a.size() != b.size()) return false;
            auto left = a.asObject().begin();
            auto right = b.asObject().begin();
            for (; left != a.asObject().end(); ++left, ++right)
                if (left->key != right->key || !identical(left->value, right->value)) return false;
            return true;
        }
    }
    return false;
}
std::string_view typeName(Type t) {
    constexpr std::string_view names[] = {"null",   "boolean", "integer", "number",
                                          "string", "array",   "object"};
    return names[static_cast<std::size_t>(t)];
}
}  // namespace supple::json
