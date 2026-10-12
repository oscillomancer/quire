// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace supple::suite::json {

// --- values ---------------------------------------------------------------------------

const Value* Object::find(std::string_view key) const {
    for (const Member& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Object::find(std::string_view key) {
    for (Member& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value& Object::set(std::string key, Value value) {
    if (Value* existing = find(key)) return *existing = std::move(value);
    members_.emplace_back(std::move(key), std::move(value));
    return members_.back().second;
}

bool Object::erase(std::string_view key) {
    const auto it = std::find_if(members_.begin(), members_.end(),
                                 [&](const Member& m) { return m.first == key; });
    if (it == members_.end()) return false;
    members_.erase(it);
    return true;
}

Value::Value(Array a) : type_(Type::Array), array_(std::make_unique<Array>(std::move(a))) {}
Value::Value(Object o) : type_(Type::Object), object_(std::make_unique<Object>(std::move(o))) {}

Value::Value(const Value& other)
    : type_(other.type_),
      bool_(other.bool_),
      int_(other.int_),
      double_(other.double_),
      string_(other.string_),
      array_(other.array_ ? std::make_unique<Array>(*other.array_) : nullptr),
      object_(other.object_ ? std::make_unique<Object>(*other.object_) : nullptr) {}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Value::~Value() = default;

long long Value::asInt() const {
    if (isInt()) return int_;
    if (isDouble() && std::isfinite(double_) && double_ == std::floor(double_) &&
        std::fabs(double_) < 9.2e18)
        return static_cast<long long>(double_);
    return 0;
}

double Value::asDouble() const {
    if (isDouble()) return double_;
    if (isInt()) return static_cast<double>(int_);
    return 0.0;
}

const std::string& Value::asString() const {
    static const std::string kEmpty;
    return isString() ? string_ : kEmpty;
}

const Array& Value::asArray() const {
    static const Array kEmpty;
    return isArray() ? *array_ : kEmpty;
}

Array& Value::asArray() {
    if (!isArray()) *this = Value(Array{});
    return *array_;
}

const Object& Value::asObject() const {
    static const Object kEmpty;
    return isObject() ? *object_ : kEmpty;
}

Object& Value::asObject() {
    if (!isObject()) *this = Value(Object{});
    return *object_;
}

Value& Value::operator[](std::string_view key) {
    Object& o = asObject();
    if (Value* v = o.find(key)) return *v;
    return o.set(std::string(key), Value());
}

void Value::push_back(Value v) { asArray().push_back(std::move(v)); }

size_t Value::size() const {
    if (isArray()) return array_->size();
    if (isObject()) return object_->size();
    return 0;
}

bool operator==(const Value& a, const Value& b) {
    if (a.isNumber() && b.isNumber()) {
        if (a.isInt() && b.isInt()) return a.int_ == b.int_;
        return a.asDouble() == b.asDouble();
    }
    if (a.type_ != b.type_) return false;
    switch (a.type_) {
        case Type::Null:
            return true;
        case Type::Bool:
            return a.bool_ == b.bool_;
        case Type::String:
            return a.string_ == b.string_;
        case Type::Array:
            return *a.array_ == *b.array_;
        case Type::Object: {
            if (a.object_->size() != b.object_->size()) return false;
            for (const Member& m : *a.object_) {
                const Value* other = b.object_->find(m.first);
                if (!other || !(m.second == *other)) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

std::string_view typeName(Type type) {
    switch (type) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return "boolean";
        case Type::Int:
            return "integer";
        case Type::Double:
            return "number";
        case Type::String:
            return "string";
        case Type::Array:
            return "array";
        case Type::Object:
            return "object";
    }
    return "value";
}

// --- parsing ----------------------------------------------------------------------------

namespace {

// Bytes in the UTF-8 sequence starting at s[i] (validated), or 0.
size_t utf8Length(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) return 1;
    size_t n = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0)
        n = 2, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0)
        n = 3, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0)
        n = 4, cp = c & 0x07;
    else
        return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (b(i + k) & 0x3F);
    }
    const uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < least[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
  public:
    Parser(std::string_view text, size_t maxDepth) : s_(text), maxDepth_(maxDepth) {}

    bool run(Value& out) {
        if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF")
            return fail("json.parse.bom", "the text starts with a byte order mark",
                        "remove the BOM; JSON is UTF-8");
        skipSpace();
        if (!value(out, 0)) return false;
        skipSpace();
        if (i_ != s_.size())
            return fail("json.parse.trailing_content", "more text after the value",
                        "send exactly one JSON value; remove what follows it");
        return true;
    }

    Error error;

  private:
    bool fail(const char* code, std::string message, std::string hint) {
        error.code = code;
        error.hint = std::move(hint);
        int line = 1;
        size_t start = 0;
        for (size_t k = 0; k < i_ && k < s_.size(); ++k)
            if (s_[k] == '\n') ++line, start = k + 1;
        error.line = line;
        error.column = static_cast<int>(i_ - start) + 1;
        size_t end = s_.find('\n', start);
        if (end == std::string_view::npos) end = s_.size();
        // A line of context around the column, and a caret under it.
        const size_t from = i_ > start + 40 ? i_ - 40 : start;
        const size_t to = std::min(end, from + 80);
        error.message = std::move(message) + " (line " + std::to_string(line) + ", column " +
                        std::to_string(error.column) + ")\n    " +
                        std::string(s_.substr(from, to - from)) + "\n    " +
                        std::string(i_ - from, ' ') + "^";
        return false;
    }

    void skipSpace() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool starts(std::string_view word) const { return s_.substr(i_, word.size()) == word; }

    bool value(Value& out, size_t depth) {
        if (depth > maxDepth_)
            return fail("json.parse.too_deep", "nested too deeply", "flatten the document");
        if (i_ >= s_.size())
            return fail("json.parse.unexpected_end", "the text ends where a value should be",
                        "complete the document");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string text;
            if (!string(text)) return false;
            out = Value(std::move(text));
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        if (starts("true")) return i_ += 4, out = Value(true), true;
        if (starts("false")) return i_ += 5, out = Value(false), true;
        if (starts("null")) return i_ += 4, out = Value(), true;
        if (c == '\'')
            return fail("json.parse.single_quotes", "single quotes",
                        "JSON strings use double quotes: \"text\"");
        if (c == '/')
            return fail("json.parse.comment", "a comment",
                        "JSON has no comments; put notes in a \"notes\" member");
        if (starts("NaN") || starts("Infinity") || starts("-Infinity"))
            return fail("json.parse.nan_or_infinity", "NaN and Infinity are not JSON",
                        "use a finite number");
        return fail("json.parse.unexpected_char", std::string("unexpected '") + c + "'",
                    "a value is an object, array, string, number, true, false or null");
    }

    bool object(Value& out, size_t depth) {
        ++i_;
        Object o;
        skipSpace();
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, out = Value(std::move(o)), true;
        for (;;) {
            skipSpace();
            if (i_ < s_.size() && s_[i_] == '}')
                return fail("json.parse.trailing_comma", "a comma before '}'",
                            "remove the last comma");
            if (i_ >= s_.size() || s_[i_] != '"') {
                if (i_ < s_.size() && s_[i_] == '\'')
                    return fail("json.parse.single_quotes", "a key in single quotes",
                                "use double quotes: \"key\"");
                return fail("json.parse.expected_key", "expected a member name in double quotes",
                            "write members as \"key\": value");
            }
            std::string key;
            if (!string(key)) return false;
            if (o.find(key))
                return fail("json.parse.duplicate_key", "\"" + key + "\" appears twice",
                            "keep one of them");
            skipSpace();
            if (i_ >= s_.size() || s_[i_] != ':')
                return fail("json.parse.expected_colon", "expected ':' after \"" + key + "\"",
                            "write \"key\": value");
            ++i_;
            skipSpace();
            Value v;
            if (!value(v, depth + 1)) return false;
            o.set(std::move(key), std::move(v));
            skipSpace();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, out = Value(std::move(o)), true;
            if (i_ >= s_.size())
                return fail("json.parse.unexpected_end", "the text ends inside an object",
                            "close it with '}'");
            return fail("json.parse.expected_comma_or_end", "expected ',' or '}' after a member",
                        "separate object members with commas");
        }
    }

    bool array(Value& out, size_t depth) {
        ++i_;
        Array a;
        skipSpace();
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, out = Value(std::move(a)), true;
        for (;;) {
            skipSpace();
            if (i_ < s_.size() && s_[i_] == ']')
                return fail("json.parse.trailing_comma", "a comma before ']'",
                            "remove the last comma");
            Value v;
            if (!value(v, depth + 1)) return false;
            a.push_back(std::move(v));
            skipSpace();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, out = Value(std::move(a)), true;
            if (i_ >= s_.size())
                return fail("json.parse.unexpected_end", "the text ends inside an array",
                            "close it with ']'");
            return fail("json.parse.expected_comma_or_end", "expected ',' or ']' after an element",
                        "separate array elements with commas");
        }
    }

    bool hex4(uint32_t& out) {
        if (i_ + 4 > s_.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_ + static_cast<size_t>(k)];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        i_ += 4;
        return true;
    }

    bool string(std::string& out) {
        ++i_;
        for (;;) {
            if (i_ >= s_.size())
                return fail("json.parse.unexpected_end", "the text ends inside a string",
                            "close it with '\"'");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') return ++i_, true;
            if (c < 0x20)
                return fail("json.parse.control_char_in_string", "a control character in a string",
                            "write it escaped: \\n, \\t or \\u00XX");
            if (c == '\\') {
                ++i_;
                if (i_ >= s_.size())
                    return fail("json.parse.unexpected_end", "the text ends in an escape", "");
                const char e = s_[i_++];
                switch (e) {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case '/':
                        out += '/';
                        break;
                    case 'b':
                        out += '\b';
                        break;
                    case 'f':
                        out += '\f';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!hex4(cp))
                            return fail("json.parse.invalid_escape", "\\u needs four hex digits",
                                        "write \\u00e9");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            uint32_t low = 0;
                            if (!starts("\\u") || (i_ += 2, !hex4(low)) || low < 0xDC00 ||
                                low > 0xDFFF)
                                return fail("json.parse.invalid_unicode", "a lone surrogate",
                                            "pair a high surrogate with a low one");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("json.parse.invalid_unicode", "a lone surrogate",
                                        "pair a high surrogate with a low one");
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default:
                        --i_;
                        return fail("json.parse.invalid_escape",
                                    std::string("\\") + e + " is not an escape",
                                    "escape only \" \\ / b f n r t and u");
                }
                continue;
            }
            const size_t n = utf8Length(s_, i_);
            if (n == 0)
                return fail("json.parse.invalid_utf8", "bytes that are not UTF-8",
                            "send UTF-8 text");
            out.append(s_.substr(i_, n));
            i_ += n;
        }
    }

    bool number(Value& out) {
        const size_t start = i_;
        if (s_[i_] == '-') ++i_;
        if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9'))
            return fail("json.parse.invalid_number", "a '-' without digits",
                        "write a number such as -3.5");
        if (s_[i_] == '0' && i_ + 1 < s_.size() && s_[i_ + 1] >= '0' && s_[i_ + 1] <= '9')
            return fail("json.parse.invalid_number", "a number with a leading zero",
                        "write 7, not 07");
        while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        bool integral = true;
        if (i_ < s_.size() && s_[i_] == '.') {
            integral = false;
            ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9'))
                return fail("json.parse.invalid_number", "a '.' without digits after it",
                            "write 1.0, not 1.");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            integral = false;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (i_ >= s_.size() || !(s_[i_] >= '0' && s_[i_] <= '9'))
                return fail("json.parse.invalid_number", "an exponent without digits", "write 1e3");
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        }
        const char* first = s_.data() + start;
        const char* last = s_.data() + i_;
        if (integral) {
            long long v = 0;
            const auto r = std::from_chars(first, last, v);
            if (r.ec == std::errc() && r.ptr == last) return out = Value(v), true;
        }
        double d = 0.0;
        const auto r = std::from_chars(first, last, d);
        if (r.ec != std::errc() || r.ptr != last || !std::isfinite(d))
            return fail("json.parse.number_out_of_range", "a number too large for a double",
                        "use a smaller number");
        out = Value(d);
        return true;
    }

    std::string_view s_;
    size_t maxDepth_;
    size_t i_ = 0;
};

}  // namespace

bool parse(std::string_view text, Value& out, Error& error, size_t maxDepth) {
    Parser p(text, maxDepth);
    Value v;
    if (!p.run(v)) {
        error = std::move(p.error);
        return false;
    }
    out = std::move(v);
    return true;
}

// --- writing ----------------------------------------------------------------------------

std::string formatDouble(double value) {
    if (!std::isfinite(value)) return "null";
    if (value == 0.0) return "0.0";
    char text[64];
    const auto r = std::to_chars(text, text + sizeof(text), value);
    std::string s(text, r.ptr);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

namespace {

void writeString(std::string& out, std::string_view s) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                } else if (c >= 0x80) {
                    const size_t n = utf8Length(s, i);
                    if (n == 0) {
                        out += "\xEF\xBF\xBD";  // U+FFFD for a byte that is not UTF-8
                        ++i;
                        continue;
                    }
                    out.append(s.substr(i, n));
                    i += n;
                    continue;
                } else {
                    out += static_cast<char>(c);
                }
        }
        ++i;
    }
    out += '"';
}

void writeScalar(std::string& out, const Value& v) {
    switch (v.type()) {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += v.asBool() ? "true" : "false";
            break;
        case Type::Int:
            out += std::to_string(v.asInt());
            break;
        case Type::Double:
            out += formatDouble(v.asDouble());
            break;
        case Type::String:
            writeString(out, v.asString());
            break;
        default:
            break;
    }
}

void writeCompactTo(std::string& out, const Value& v) {
    if (v.isArray()) {
        out += '[';
        bool first = true;
        for (const Value& e : v.asArray()) {
            if (!first) out += ',';
            first = false;
            writeCompactTo(out, e);
        }
        out += ']';
    } else if (v.isObject()) {
        out += '{';
        bool first = true;
        for (const Member& m : v.asObject()) {
            if (!first) out += ',';
            first = false;
            writeString(out, m.first);
            out += ':';
            writeCompactTo(out, m.second);
        }
        out += '}';
    } else {
        writeScalar(out, v);
    }
}

constexpr size_t kLineWidth = 100;

// `prefix` is what precedes the value on its line (indent and key), for the
// one-line rule of scalar arrays.
void writePretty(std::string& out, const Value& v, int indent, size_t prefix) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    const std::string inner(static_cast<size_t>(indent + 1) * 2, ' ');
    if (v.isArray()) {
        const Array& a = v.asArray();
        if (a.empty()) return void(out += "[]");
        const bool scalars = std::all_of(
            a.begin(), a.end(), [](const Value& e) { return !e.isArray() && !e.isObject(); });
        if (scalars) {
            std::string line;
            writeCompactTo(line, v);
            // Compact puts no space after commas; the one-line form has one.
            std::string spaced;
            bool inString = false, escaped = false;
            for (char c : line) {
                spaced += c;
                if (inString) {
                    if (escaped)
                        escaped = false;
                    else if (c == '\\')
                        escaped = true;
                    else if (c == '"')
                        inString = false;
                } else if (c == '"') {
                    inString = true;
                } else if (c == ',') {
                    spaced += ' ';
                }
            }
            if (prefix + spaced.size() + 1 <= kLineWidth) return void(out += spaced);
        }
        out += "[\n";
        for (size_t k = 0; k < a.size(); ++k) {
            out += inner;
            writePretty(out, a[k], indent + 1, inner.size());
            out += k + 1 < a.size() ? ",\n" : "\n";
        }
        out += pad + "]";
    } else if (v.isObject()) {
        const Object& o = v.asObject();
        if (o.empty()) return void(out += "{}");
        out += "{\n";
        size_t k = 0;
        for (const Member& m : o) {
            out += inner;
            const size_t before = out.size();
            writeString(out, m.first);
            out += ": ";
            writePretty(out, m.second, indent + 1, inner.size() + (out.size() - before));
            out += ++k < o.size() ? ",\n" : "\n";
        }
        out += pad + "}";
    } else {
        writeScalar(out, v);
    }
}

}  // namespace

std::string writeCompact(const Value& value) {
    std::string out;
    writeCompactTo(out, value);
    return out;
}

std::string writeCanonical(const Value& value) {
    std::string out;
    writePretty(out, value, 0, 0);
    out += '\n';
    return out;
}

// --- merge patches and pointers -------------------------------------------------------------

Value mergePatch(const Value& target, const Value& patch) {
    if (!patch.isObject()) return patch;
    Value result = target.isObject() ? target : Value::object();
    for (const Member& m : patch.asObject()) {
        if (m.second.isNull()) {
            result.asObject().erase(m.first);
        } else {
            const Value* existing = result.find(m.first);
            result.asObject().set(m.first, mergePatch(existing ? *existing : Value(), m.second));
        }
    }
    return result;
}

std::string pointerEscape(std::string_view token) {
    std::string out;
    for (char c : token) {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

std::string pointerJoin(std::string_view base, std::string_view key) {
    return std::string(base) + "/" + pointerEscape(key);
}

namespace {
Value errorValue(const Error& e) {
    Value v = Value::object();
    v["code"] = e.code;
    v["message"] = e.message;
    v["path"] = e.path;
    v["hint"] = e.hint;
    if (!e.details.empty()) {
        Value details = Value::array();
        for (const Error& d : e.details) details.push_back(errorValue(d));
        v["details"] = std::move(details);
    }
    return v;
}
}  // namespace

std::string errorsToJson(const std::vector<Error>& errors) {
    Value list = Value::array();
    for (const Error& e : errors) list.push_back(errorValue(e));
    return writeCompact(list);
}

}  // namespace supple::suite::json
