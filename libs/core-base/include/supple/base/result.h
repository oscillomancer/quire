// Structured errors and Result<T>, used by every library API.
//
// Errors are designed to be read by an LLM or a script as well as a person:
// a stable machine code, a message, where it happened, and how to fix it.
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace supple {

struct SourceLoc {
    int line = 0;    // 1-based
    int column = 0;  // 1-based, in bytes
};

struct Error {
    // Stable, dotted, lowercase machine code: "json.parse.unexpected_char",
    // "schema.out_of_range", "command.unknown". Callers switch on this.
    std::string code;
    // One sentence describing what is wrong, in terms of the caller's input.
    std::string message;
    // Where: a JSON Pointer (RFC 6901) into the offending document, or empty.
    std::string path;
    // How to fix it: the valid range, the closest valid name, an example.
    std::string hint;
    // Text position for errors in textual input (files, notation, JSON text).
    std::optional<SourceLoc> loc;
    // Further errors when several things are wrong at once (validation).
    std::vector<Error> details;

    Error() = default;
    Error(std::string c, std::string m, std::string p = {}, std::string h = {})
        : code(std::move(c)), message(std::move(m)), path(std::move(p)), hint(std::move(h)) {}

    Error& at(std::string p) & {
        path = std::move(p);
        return *this;
    }
    Error&& at(std::string p) && {
        path = std::move(p);
        return std::move(*this);
    }
    Error& withHint(std::string h) & {
        hint = std::move(h);
        return *this;
    }
    Error&& withHint(std::string h) && {
        hint = std::move(h);
        return std::move(*this);
    }
    Error& withLoc(SourceLoc l) & {
        loc = l;
        return *this;
    }
    Error&& withLoc(SourceLoc l) && {
        loc = l;
        return std::move(*this);
    }

    // "code at path (line:col): message. Hint: hint" -- for logs and CLIs.
    std::string toString() const;
};

// Result<T>: a value or an Error. Minimal std::expected stand-in (C++20).
template <typename T>
class [[nodiscard]] Result {
  public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & {
        assert(ok());
        return std::get<0>(v_);
    }
    const T& value() const& {
        assert(ok());
        return std::get<0>(v_);
    }
    T&& value() && {
        assert(ok());
        return std::get<0>(std::move(v_));
    }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    const Error& error() const& {
        assert(!ok());
        return std::get<1>(v_);
    }
    Error&& error() && {
        assert(!ok());
        return std::get<1>(std::move(v_));
    }

    T valueOr(T fallback) const& { return ok() ? value() : std::move(fallback); }

  private:
    std::variant<T, Error> v_;
};

// Result<void>
template <>
class [[nodiscard]] Result<void> {
  public:
    Result() = default;
    Result(Error error) : e_(std::move(error)) {}
    bool ok() const { return !e_.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const& {
        assert(!ok());
        return *e_;
    }
    Error&& error() && {
        assert(!ok());
        return std::move(*e_);
    }

  private:
    std::optional<Error> e_;
};

using Status = Result<void>;

inline Status okStatus() { return {}; }

// Propagate an error from an expression yielding Result<...>.
#define SUPPLE_TRY(expr)                                                  \
    do {                                                                  \
        auto&& supple_try_r_ = (expr);                                    \
        if (!supple_try_r_.ok()) return std::move(supple_try_r_).error(); \
    } while (0)

}  // namespace supple
