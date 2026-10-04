#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace supple::json {
enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };
class Value;
struct Member;
class Object;
using Array = std::vector<Value>;

class Value {
  public:
    Value();
    Value(std::nullptr_t);
    Value(bool v);
    Value(int v);
    Value(long v);
    Value(long long v);
    Value(unsigned v);
    Value(unsigned long v);
    Value(unsigned long long v);
    Value(double v);
    Value(const char* v);
    Value(std::string v);
    Value(std::string_view v);
    Value(Array v);
    Value(Object v);
    Value(const Value&);
    Value& operator=(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(Value&&) noexcept;
    ~Value();
    static Value array();
    static Value object();
    Type type() const;
    bool isNull() const;
    bool isBool() const;
    bool isInt() const;
    bool isDouble() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
    bool asBool() const;
    std::int64_t asInt() const;
    double asDouble() const;
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();
    const Value* find(std::string_view key) const;
    Value& operator[](std::string_view key);
    const Value& at(std::size_t i) const;
    void push_back(Value v);
    std::size_t size() const;
    friend bool operator==(const Value&, const Value&);

  private:
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                                 std::unique_ptr<Array>, std::unique_ptr<Object>>;
    Storage data_;
};

// An object member. The key is const: iterating a mutable Object gives
// write access to values only, so the member index can never go stale.
struct Member {
    const std::string key;
    Value value;
};
// Members keep insertion order. Objects with many members carry a key index
// that every mutating operation maintains eagerly; const operations never
// write, so a const Object is safe to read from several threads.
class Object {
  public:
    Object() = default;
    Object(const Object&) = default;
    Object(Object&& other) noexcept;
    Object& operator=(const Object& other);
    Object& operator=(Object&& other) noexcept;
    ~Object() = default;
    std::size_t size() const;
    bool empty() const;
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const;
    Value& set(std::string key, Value v);
    bool erase(std::string_view key);
    Value& operator[](std::string_view key);
    auto begin() { return members_.begin(); }
    auto end() { return members_.end(); }
    auto begin() const { return members_.begin(); }
    auto end() const { return members_.end(); }
    void sortKeys();

  private:
    // Members are never move-assigned (the key is const), so growth and
    // removal rebuild the vector by construction instead.
    std::vector<Member> members_;
    // Key -> position; complete when members_.size() >= kIndexMin, else empty.
    std::unordered_map<std::string, std::size_t> index_;
    Value& append(std::string key, Value v);
    void rebuildIndex();
};
// Unlike operator==, compares JSON types, object order, and double bits.
bool identical(const Value& a, const Value& b);
std::string_view typeName(Type type);
}  // namespace supple::json
