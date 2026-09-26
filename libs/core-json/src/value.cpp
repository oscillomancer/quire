#include <supple/json/value.h>
#include <algorithm>
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
        if (std::isfinite(d) && std::trunc(d) == d && d >= -0x1p63 && d < 0x1p63)
            return static_cast<std::int64_t>(d);
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
std::size_t Object::size() const { return members_.size(); }
bool Object::empty() const { return members_.empty(); }
void Object::ensureIndex() const {
    if (indexValid_) return;
    index_.clear();
    for (std::size_t i = 0; i < members_.size(); ++i) index_.emplace(members_[i].key, i);
    indexValid_ = true;
}
const Value* Object::find(std::string_view key) const {
    if (members_.size() >= 16) {
        ensureIndex();
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
    members_.push_back({std::move(key), std::move(v)});
    indexValid_ = false;
    return members_.back().value;
}
bool Object::erase(std::string_view key) {
    auto it = std::find_if(members_.begin(), members_.end(),
                           [key](const Member& m) { return m.key == key; });
    if (it == members_.end()) return false;
    members_.erase(it);
    indexValid_ = false;
    return true;
}
Value& Object::operator[](std::string_view key) {
    if (auto* p = find(key)) return *p;
    members_.push_back({std::string(key), {}});
    indexValid_ = false;
    return members_.back().value;
}
void Object::sortKeys() {
    indexValid_ = false;
    std::sort(members_.begin(), members_.end(),
              [](const Member& a, const Member& b) { return a.key < b.key; });
}
bool operator==(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.asInt() == b.asInt();
        return (a.isInt() ? static_cast<long double>(a.asInt())
                          : static_cast<long double>(a.asDouble())) ==
               (b.isInt() ? static_cast<long double>(b.asInt())
                          : static_cast<long double>(b.asDouble()));
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
std::string_view typeName(Type t) {
    constexpr std::string_view names[] = {"null",   "boolean", "integer", "number",
                                          "string", "array",   "object"};
    return names[static_cast<std::size_t>(t)];
}
}  // namespace supple::json
