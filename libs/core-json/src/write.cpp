// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/json/utf8.h>
#include <supple/json/write.h>
#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <optional>
#include <set>
namespace supple::json {
std::string formatDouble(double d) {
    if (d == 0) return "0.0";
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), d);
    if (ec != std::errc{}) return {};
    std::string out(buf, end);
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}
namespace {
// A string's bytes that do not form UTF-8 are written as U+FFFD, one per
// invalid byte, so whatever is written parses back.
void quote(std::string& out, std::string_view s) {
    static constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            if (const std::size_t n = utf8SequenceLength(s, i)) {
                out.append(s.substr(i, n));
                i += n - 1;
            } else {
                out += "\xEF\xBF\xBD";
            }
            continue;
        }
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
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else
                    out += static_cast<char>(c);
        }
    }
    out += '"';
}
class Writer {
  public:
    // keepFirst: of keys that write the same, keep the first member instead of failing.
    explicit Writer(const WriteOptions& o, bool keepFirst = false)
        : opt_(o), keepFirst_(keepFirst) {}
    Result<std::string> run(const Value& v) {
        if (!emit(v, 0))
            return error_ ? *error_
                          : Error("json.write.not_finite", "cannot write NaN or infinity", {},
                                  "use a finite number");
        if (opt_.pretty) out_ += '\n';
        return out_;
    }

  private:
    const WriteOptions& opt_;
    const bool keepFirst_;
    std::optional<Error> error_;
    std::string out_;
    // Keys that differ only in bytes that are not UTF-8 can write the same
    // (each such byte becomes U+FFFD), which would not parse back.
    bool distinctKeys(std::vector<const Member*>& members) {
        if (std::none_of(members.begin(), members.end(), [](const Member* m) {
                return firstInvalidUtf8(m->key) != std::string_view::npos;
            }))
            return true;
        std::set<std::string> written;
        std::vector<const Member*> kept;
        for (const Member* m : members) {
            std::string key;
            quote(key, m->key);
            if (written.insert(key).second) {
                kept.push_back(m);
            } else if (!keepFirst_) {
                error_ = Error("json.write.duplicate_key",
                               "two keys both write as " + key +
                                   " once their bytes that are not UTF-8 are replaced",
                               {}, "make the keys UTF-8, or distinct in their UTF-8 parts");
                return false;
            }
        }
        members = std::move(kept);
        return true;
    }
    void spaces(int n) { out_.append(static_cast<std::size_t>(std::max(0, n)), ' '); }
    bool emit(const Value& v, int depth) {
        switch (v.type()) {
            case Type::Null:
                out_ += "null";
                return true;
            case Type::Bool:
                out_ += v.asBool() ? "true" : "false";
                return true;
            case Type::Int:
                out_ += std::to_string(v.asInt());
                return true;
            case Type::Double:
                if (!std::isfinite(v.asDouble())) return false;
                out_ += formatDouble(v.asDouble());
                return true;
            case Type::String:
                quote(out_, v.asString());
                return true;
            case Type::Array: {
                const auto& a = v.asArray();
                out_ += '[';
                if (a.empty()) {
                    out_ += ']';
                    return true;
                }
                bool scalar = std::all_of(a.begin(), a.end(), [](const Value& x) {
                    return !x.isArray() && !x.isObject();
                });
                bool inlineArray = !opt_.pretty || scalar;
                if (opt_.pretty && scalar) {
                    WriteOptions compactOptions{false, false, opt_.indent, opt_.maxLineWidth};
                    Writer compact(compactOptions, keepFirst_);
                    auto trial = compact.run(v);
                    // The one-line pretty form is the compact form plus one space
                    // after each of the (size - 1) separating commas. The opening
                    // bracket is already in out_ and also counted in the trial.
                    const std::size_t lineStart =
                        out_.rfind('\n') == std::string::npos ? 0 : out_.rfind('\n') + 1;
                    const std::size_t column = out_.size() - lineStart - 1;
                    if (!trial || column + trial->size() + (a.size() - 1) >
                                      static_cast<std::size_t>(opt_.maxLineWidth))
                        inlineArray = false;
                }
                for (std::size_t i = 0; i < a.size(); ++i) {
                    if (i) out_ += ',';
                    if (opt_.pretty) {
                        if (inlineArray) {
                            if (i) out_ += ' ';
                        } else {
                            out_ += '\n';
                            spaces((depth + 1) * opt_.indent);
                        }
                    }
                    if (!emit(a[i], depth + 1)) return false;
                }
                if (opt_.pretty && !inlineArray) {
                    out_ += '\n';
                    spaces(depth * opt_.indent);
                }
                out_ += ']';
                return true;
            }
            case Type::Object: {
                out_ += '{';
                const auto& o = v.asObject();
                if (o.empty()) {
                    out_ += '}';
                    return true;
                }
                std::vector<const Member*> members;
                for (const auto& m : o) members.push_back(&m);
                if (!distinctKeys(members)) return false;
                if (opt_.sortKeys)
                    std::sort(members.begin(), members.end(),
                              [](const Member* a, const Member* b) { return a->key < b->key; });
                for (std::size_t i = 0; i < members.size(); ++i) {
                    if (i) out_ += ',';
                    if (opt_.pretty) {
                        out_ += '\n';
                        spaces((depth + 1) * opt_.indent);
                    }
                    quote(out_, members[i]->key);
                    out_ += opt_.pretty ? ": " : ":";
                    if (!emit(members[i]->value, depth + 1)) return false;
                }
                if (opt_.pretty) {
                    out_ += '\n';
                    spaces(depth * opt_.indent);
                }
                out_ += '}';
                return true;
            }
        }
        return false;
    }
};
Value finiteOrNull(const Value& input) {
    if (input.isDouble() && !std::isfinite(input.asDouble())) return {};
    if (input.isArray()) {
        Value result = Value::array();
        for (const auto& item : input.asArray()) result.push_back(finiteOrNull(item));
        return result;
    }
    if (input.isObject()) {
        Value result = Value::object();
        for (const auto& member : input.asObject()) result[member.key] = finiteOrNull(member.value);
        return result;
    }
    return input;
}
// writeCompact's and writeCanonical's: whatever they write parses back.
std::string writeAnyway(const Value& v, const WriteOptions& o) {
    auto r = Writer(o, true).run(v);
    assert(r);  // only a non-finite number fails it
    return r ? *r : *Writer(o, true).run(finiteOrNull(v));
}
}  // namespace
Result<std::string> write(const Value& v, const WriteOptions& o) { return Writer(o).run(v); }
std::string writeCompact(const Value& v) { return writeAnyway(v, {false}); }
std::string writeCanonical(const Value& v) { return writeAnyway(v, {}); }
}  // namespace supple::json
