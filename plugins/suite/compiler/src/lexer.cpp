// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Tokens, as C++ forms them, for the subset: every literal is read with C++'s
// rules so it has the value and type a C++ compiler would give it, and what
// the subset has no use for (unsigned and long literals, raw strings, macros)
// is reported where it is written.
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

#include "ast.h"

namespace supple::scriptc {

void Diagnostics::error(Loc loc, std::string code, std::string message, std::string hint) {
    if (errors_ >= kMaxErrors) return;
    ++errors_;
    list_.push_back(
        {Diagnostic::Severity::Error, std::move(code), std::move(message), std::move(hint), loc});
    if (errors_ == kMaxErrors)
        list_.push_back({Diagnostic::Severity::Error, "script.too_many_errors",
                         "too many errors; stopping here", "fix the first errors and compile again",
                         loc});
}

void Diagnostics::warning(Loc loc, std::string code, std::string message, std::string hint) {
    list_.push_back(
        {Diagnostic::Severity::Warning, std::move(code), std::move(message), std::move(hint), loc});
}

namespace {

// Every C++20 keyword, so a script using one the subset lacks hears why.
constexpr std::string_view kKeywords[] = {
    "alignas",       "alignof",     "asm",       "auto",      "bool",         "break",
    "case",          "catch",       "char",      "char8_t",   "char16_t",     "char32_t",
    "class",         "concept",     "const",     "consteval", "constexpr",    "constinit",
    "const_cast",    "continue",    "co_await",  "co_return", "co_yield",     "decltype",
    "default",       "delete",      "do",        "double",    "dynamic_cast", "else",
    "enum",          "explicit",    "export",    "extern",    "false",        "float",
    "for",           "friend",      "goto",      "if",        "inline",       "int",
    "long",          "mutable",     "namespace", "new",       "noexcept",     "nullptr",
    "operator",      "private",     "protected", "public",    "register",     "reinterpret_cast",
    "requires",      "return",      "short",     "signed",    "sizeof",       "static",
    "static_assert", "static_cast", "struct",    "switch",    "template",     "this",
    "thread_local",  "throw",       "true",      "try",       "typedef",      "typeid",
    "typename",      "union",       "unsigned",  "using",     "virtual",      "void",
    "volatile",      "wchar_t",     "while"};

// C++'s alternative spellings of operators.
struct Alternative {
    std::string_view word, punct;
};
constexpr Alternative kAlternatives[] = {{"and", "&&"},    {"or", "||"},    {"not", "!"},
                                         {"not_eq", "!="}, {"bitand", "&"}, {"bitor", "|"},
                                         {"xor", "^"},     {"compl", "~"},  {"and_eq", "&="},
                                         {"or_eq", "|="},  {"xor_eq", "^="}};

// Longest first.
constexpr std::string_view kPuncts[] = {
    "<=>", "<<=", ">>=", "...", "->*", "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==",
    "!=",  "&&",  "||",  "+=",  "-=",  "*=", "/=", "%=", "&=", "|=", "^=", ".*", "{",  "}",
    "[",   "]",   "(",   ")",   ";",   ":",  ",",  ".",  "?",  "~",  "!",  "+",  "-",  "*",
    "/",   "%",   "^",   "&",   "|",   "=",  "<",  ">",  "#",  "@",  "$",  "`",  "\\"};

bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

class Lexer {
  public:
    Lexer(std::string_view src, Diagnostics& d) : src_(src), diags_(d) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        // C++ removes escaped newlines before recognizing comments and
        // tokens. Refuse them even in comments, where otherwise the two
        // compilers could execute different source text.
        for (size_t i = 0; i + 1 < src_.size(); ++i)
            if (src_[i] == '\\' &&
                (src_[i + 1] == '\n' ||
                 (src_[i + 1] == '\r' && i + 2 < src_.size() && src_[i + 2] == '\n'))) {
                Loc loc{1, 1};
                for (size_t j = 0; j < i; ++j) {
                    if (src_[j] == '\n')
                        ++loc.line, loc.column = 1;
                    else
                        ++loc.column;
                }
                diags_.error(loc, "script.lex.continuation",
                             "escaped newlines are not in the subset",
                             "remove the backslash at the end of the line");
                break;
            }
        bool lineStart = true;
        while (true) {
            skipSpace(lineStart);
            if (pos_ >= src_.size()) break;
            Token t;
            t.loc = here();
            const char c = src_[pos_];
            if (c == '#' && lineStart) {
                t.kind = Tok::Hash;
                const size_t start = pos_;
                while (pos_ < src_.size() && src_[pos_] != '\n') {
                    if (src_[pos_] == '\\' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '\n')
                        diags_.error(here(), "script.lex.continuation",
                                     "a preprocessor line cannot continue onto the next one",
                                     "the only preprocessor line a script has is #include "
                                     "<supple/script.h>");
                    advance();
                }
                t.text = std::string(src_.substr(start, pos_ - start));
                out.push_back(std::move(t));
                continue;
            }
            lineStart = false;
            if (isIdentStart(c)) {
                lexIdent(t);
            } else if (isDigit(c) ||
                       (c == '.' && pos_ + 1 < src_.size() && isDigit(src_[pos_ + 1]))) {
                lexNumber(t);
            } else if (c == '"' || ((c == 'u' || c == 'U' || c == 'L') && peek(1) == '"')) {
                lexString(t);
            } else if (c == '\'') {
                lexChar(t);
            } else {
                lexPunct(t);
            }
            out.push_back(std::move(t));
        }
        Token end;
        end.kind = Tok::End;
        end.loc = here();
        out.push_back(end);
        return out;
    }

  private:
    Loc here() const { return Loc{line_, column_}; }
    char peek(size_t k) const { return pos_ + k < src_.size() ? src_[pos_ + k] : '\0'; }
    void advance() {
        if (src_[pos_] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        ++pos_;
    }

    void skipSpace(bool& lineStart) {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') {
                lineStart = true;
                advance();
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                const Loc start = here();
                advance();
                advance();
                bool closed = false;
                while (pos_ < src_.size()) {
                    if (src_[pos_] == '*' && peek(1) == '/') {
                        advance();
                        advance();
                        closed = true;
                        break;
                    }
                    advance();
                }
                if (!closed)
                    diags_.error(start, "script.lex.comment", "this comment is never closed",
                                 "end it with */");
            } else {
                break;
            }
        }
    }

    void lexIdent(Token& t) {
        const size_t start = pos_;
        while (pos_ < src_.size() && isIdentChar(src_[pos_])) advance();
        t.text = std::string(src_.substr(start, pos_ - start));
        // A raw string: R"(...)".
        if ((t.text == "R" || t.text == "u8R" || t.text == "LR" || t.text == "uR" ||
             t.text == "UR") &&
            pos_ < src_.size() && src_[pos_] == '"') {
            diags_.error(t.loc, "script.lex.raw_string",
                         "raw string literals are not in the subset",
                         "write an ordinary string literal: \"...\"");
            Token ignored;
            lexString(ignored);
            t.kind = Tok::StringLit;
            return;
        }
        for (const auto& a : kAlternatives)
            if (t.text == a.word) {
                t.kind = Tok::Punct;
                t.text = std::string(a.punct);
                return;
            }
        for (std::string_view k : kKeywords)
            if (t.text == k) {
                t.kind = Tok::Keyword;
                return;
            }
        t.kind = Tok::Ident;
    }

    void lexNumber(Token& t) {
        const size_t start = pos_;
        // Scan C++'s pp-number: digits, letters, dots, digit separators and
        // signs after an exponent letter.
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (isIdentChar(c) || c == '.' || (c == '\'' && isIdentChar(peek(1)))) {
                advance();
            } else if ((c == '+' || c == '-') && pos_ > start) {
                const char prev = src_[pos_ - 1];
                const bool hex = src_.substr(start, 2) == "0x" || src_.substr(start, 2) == "0X";
                if ((!hex && (prev == 'e' || prev == 'E')) || (hex && (prev == 'p' || prev == 'P')))
                    advance();
                else
                    break;
            } else {
                break;
            }
        }
        t.text = std::string(src_.substr(start, pos_ - start));
        const bool hexSpelling = t.text.starts_with("0x") || t.text.starts_with("0X");
        auto digit = [&](char c) {
            return isDigit(c) ||
                   (hexSpelling && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        };
        for (size_t i = 0; i < t.text.size(); ++i)
            if (t.text[i] == '\'' && (i == 0 || i + 1 == t.text.size() || !digit(t.text[i - 1]) ||
                                      !digit(t.text[i + 1])))
                diags_.error(t.loc, "script.lex.number", "a digit separator must be between digits",
                             "write separators between digits, as in 1'000 or 0xA'B");
        std::string digits;
        for (char c : t.text)
            if (c != '\'') digits += c;

        const bool hex =
            digits.size() > 1 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
        const bool binary =
            digits.size() > 1 && digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B');
        const bool isFloat =
            !hex && !binary &&
            (digits.find('.') != std::string::npos || digits.find('e') != std::string::npos ||
             digits.find('E') != std::string::npos);
        if (hex &&
            (digits.find('.') != std::string::npos || digits.find('p') != std::string::npos ||
             digits.find('P') != std::string::npos)) {
            diags_.error(t.loc, "script.lex.hex_float",
                         "hexadecimal floating literals are not in the subset",
                         "write the number in decimal");
            t.kind = Tok::FloatLit;
            return;
        }
        if (isFloat) {
            t.kind = Tok::FloatLit;
            std::string body = digits;
            char suffix = body.back();
            if (suffix == 'f' || suffix == 'F') {
                t.floatSuffix = true;
                body.pop_back();
            } else if (suffix == 'l' || suffix == 'L') {
                diags_.error(t.loc, "script.lex.long_double",
                             "long double literals are not in the subset",
                             "drop the L: a plain literal is a double");
                body.pop_back();
            }
            for (char c : body)
                if (!(isDigit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) {
                    diags_.error(t.loc, "script.lex.number", "'" + t.text + "' is not a number",
                                 "write a decimal number such as 1.5, 2e-3 or 0.25f");
                    return;
                }
            // strtod reads C's decimal form exactly as C++ does: correctly
            // rounded to double. A float literal is rounded once more, to
            // float, as C++ rounds the literal's value (from the decimal,
            // directly): strtof, not (float)strtod, to avoid double rounding.
            errno = 0;
            char* end = nullptr;
            if (t.floatSuffix) {
                const float f = std::strtof(body.c_str(), &end);
                t.floatValue = static_cast<double>(f);
            } else {
                t.floatValue = std::strtod(body.c_str(), &end);
            }
            if (end != body.c_str() + body.size())
                diags_.error(t.loc, "script.lex.number", "'" + t.text + "' is not a number",
                             "write a decimal number such as 1.5, 2e-3 or 0.25f");
            if (std::isinf(t.floatValue))
                diags_.error(t.loc, "script.lex.range",
                             "'" + t.text + "' is too large to represent",
                             t.floatSuffix ? "a float reaches about 3.4e38"
                                           : "a double reaches about 1.8e308");
            return;
        }
        t.kind = Tok::IntLit;
        // Suffixes: none of them name a type the subset has.
        size_t end = digits.size();
        while (end > 0 &&
               (digits[end - 1] == 'u' || digits[end - 1] == 'U' || digits[end - 1] == 'l' ||
                digits[end - 1] == 'L' || digits[end - 1] == 'z' || digits[end - 1] == 'Z'))
            --end;
        if (end != digits.size()) {
            diags_.error(
                t.loc, "script.lex.int_suffix",
                "'" + t.text +
                    "' is an unsigned or long literal; the subset's only integer type is int",
                "drop the suffix");
        }
        std::string body = digits.substr(0, end);
        int base = 10;
        size_t from = 0;
        if (hex) {
            base = 16;
            from = 2;
        } else if (binary) {
            base = 2;
            from = 2;
        } else if (body.size() > 1 && body[0] == '0') {
            diags_.error(t.loc, "script.lex.octal",
                         "'" + t.text + "' starts with 0, which makes it an octal number in C++",
                         "drop the leading zeros");
            base = 8;
            from = 1;
        }
        unsigned long long value = 0;
        bool overflow = false;
        if (from >= body.size()) {
            diags_.error(t.loc, "script.lex.number", "'" + t.text + "' is not a number",
                         "write digits with an optional '.', exponent or f: 0.5, 1e-3, 2.0f");
            return;
        }
        for (size_t i = from; i < body.size(); ++i) {
            const char c = body[i];
            int d = -1;
            if (isDigit(c))
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            if (d < 0 || d >= base) {
                diags_.error(t.loc, "script.lex.number", "'" + t.text + "' is not a number",
                             "write a decimal integer such as 512");
                return;
            }
            if (value > (~0ull - static_cast<unsigned>(d)) / static_cast<unsigned>(base))
                overflow = true;
            value = value * static_cast<unsigned>(base) + static_cast<unsigned>(d);
        }
        // A literal that does not fit in int would be a long (or, in hex, an
        // unsigned int) in C++: a type the subset does not have.
        if (overflow || value > 2147483647ull) {
            diags_.error(
                t.loc, "script.lex.int_range",
                "'" + t.text + "' does not fit in an int (the largest is 2147483647)",
                "write it as a double, such as " + body + ".0, if a large number is what you mean");
            value = 0;
        }
        t.intValue = static_cast<std::int64_t>(value);
    }

    void lexString(Token& t) {
        t.kind = Tok::StringLit;
        if (src_[pos_] != '"') {
            diags_.error(here(), "script.lex.wide_string",
                         "wide and Unicode string literals are not in the subset",
                         "write an ordinary string literal: \"...\" (it may hold UTF-8)");
            advance();
        }
        const size_t start = pos_;
        advance();  // the opening quote
        std::string value;
        bool closed = false;
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == '\n') break;
            if (c == '"') {
                advance();
                closed = true;
                break;
            }
            if (c == '\\') {
                advance();
                if (pos_ >= src_.size()) break;
                const char e = src_[pos_];
                switch (e) {
                    case 'n':
                        value += '\n';
                        break;
                    case 't':
                        value += '\t';
                        break;
                    case 'r':
                        value += '\r';
                        break;
                    case '0':
                        if (peek(1) >= '0' && peek(1) <= '7')
                            diags_.error(here(), "script.lex.escape",
                                         "octal string escapes are not in the subset",
                                         "use \\n or \\t, or write the character itself (UTF-8)");
                        value += '\0';
                        break;
                    case '\\':
                        value += '\\';
                        break;
                    case '"':
                        value += '"';
                        break;
                    case '\'':
                        value += '\'';
                        break;
                    default:
                        diags_.error(
                            here(), "script.lex.escape",
                            std::string("the escape \\") + e + " is not in the subset",
                            "use \\n, \\t, \\\\, \\\" or write the character itself (UTF-8)");
                }
                advance();
                continue;
            }
            value += c;
            advance();
        }
        if (!closed)
            diags_.error(t.loc, "script.lex.string", "this string literal is never closed",
                         "end it with \"");
        t.text = std::string(src_.substr(start, pos_ - start));
        t.stringValue = std::move(value);
    }

    void lexChar(Token& t) {
        t.kind = Tok::CharLit;
        const size_t start = pos_;
        advance();
        while (pos_ < src_.size() && src_[pos_] != '\'' && src_[pos_] != '\n') {
            if (src_[pos_] == '\\') advance();
            if (pos_ < src_.size()) advance();
        }
        if (pos_ < src_.size() && src_[pos_] == '\'') advance();
        t.text = std::string(src_.substr(start, pos_ - start));
    }

    void lexPunct(Token& t) {
        t.kind = Tok::Punct;
        for (std::string_view p : kPuncts)
            if (src_.substr(pos_, p.size()) == p) {
                t.text = std::string(p);
                for (size_t i = 0; i < p.size(); ++i) advance();
                return;
            }
        // Anything else (a stray byte, a non-ASCII character outside a
        // string): report it once and move on.
        t.text = std::string(1, src_[pos_]);
        diags_.error(here(), "script.lex.character", "unexpected character '" + t.text + "'",
                     "only ASCII is allowed outside strings and comments");
        advance();
    }

    std::string_view src_;
    Diagnostics& diags_;
    size_t pos_ = 0;
    int line_ = 1, column_ = 1;
};

}  // namespace

std::vector<Token> lex(std::string_view source, Diagnostics& diags) {
    return Lexer(source, diags).run();
}

}  // namespace supple::scriptc
