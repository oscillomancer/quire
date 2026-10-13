// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The compiler's front door: source text in, a Program and diagnostics out.
#include <cstdio>

#include "program.h"
#include "sema.h"
#include "supple/script_compiler.hpp"

namespace supple::scriptc {

Result compile(std::string_view source, const Options& options) {
    Result result;
    Diagnostics diags;
    Unit unit;
    if (source.size() > (4u << 20)) {
        diags.error(
            Loc{1, 1}, "script.too_large", "the script is larger than 4 MiB",
            "keep a script under 1 MiB: compute tables in prepare() rather than writing them out");
    } else {
        std::vector<Token> tokens = lex(source, diags);
        parse(tokens, unit, diags);
        if (!diags.hasErrors()) {
            Analysis analysis;
            analyse(unit, analysis, diags);
            if (!diags.hasErrors()) {
                CompileOptions co;
                co.interpret = options.interpret;
                co.optimize = options.optimize;
                co.listing = options.listing;
                result.program = generate(unit, analysis, co, diags);
                if (diags.hasErrors()) result.program.reset();
            }
        }
    }
    for (const Diagnostic& d : diags.list())
        result.diagnostics.push_back(Message{d.severity == Diagnostic::Severity::Error, d.code,
                                             d.message, d.hint, d.loc.line, d.loc.column});
    return result;
}

namespace {
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
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
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out + "\"";
}
}  // namespace

std::string toJson(const std::vector<Message>& diagnostics) {
    std::string out = "[";
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        const Message& m = diagnostics[i];
        if (i) out += ",";
        out += "{\"severity\":" + quoted(m.error ? "error" : "warning") +
               ",\"code\":" + quoted(m.code) + ",\"message\":" + quoted(m.message) +
               ",\"hint\":" + quoted(m.hint) + ",\"line\":" + std::to_string(m.line) +
               ",\"column\":" + std::to_string(m.column) + "}";
    }
    return out + "]";
}

std::string toText(const std::vector<Message>& diagnostics, std::string_view file) {
    std::string out;
    for (const Message& m : diagnostics) {
        out += std::string(file) + ":" + std::to_string(m.line) + ":" + std::to_string(m.column) +
               ": " + (m.error ? "error: " : "warning: ") + m.message + " [" + m.code + "]\n";
        if (!m.hint.empty()) out += "    hint: " + m.hint + "\n";
    }
    return out;
}

}  // namespace supple::scriptc
