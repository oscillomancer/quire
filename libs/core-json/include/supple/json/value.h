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

struct Member {
    std::string key;
    Value value;
};
class Object {
  public:
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
    std::vector<Member> members_;
    mutable std::unordered_map<std::string, std::size_t> index_;
    mutable bool indexValid_ = false;
    void ensureIndex() const;
};
std::string_view typeName(Type type);
}  // namespace supple::json
