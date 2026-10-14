// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/json/pointer.h>
#include <charconv>
namespace supple::json {
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
std::string pointerJoin(std::string_view base, std::size_t index) {
    return pointerJoin(base, std::to_string(index));
}
Result<std::vector<std::string>> pointerSplit(std::string_view pointer) {
    std::vector<std::string> tokens;
    if (pointer.empty()) return tokens;
    if (pointer[0] != '/')
        return Error("json.pointer.invalid", "JSON Pointer must start with '/'", {},
                     "use / followed by escaped tokens");
    for (std::size_t i = 1; i <= pointer.size();) {
        std::string token;
        while (i < pointer.size() && pointer[i] != '/') {
            if (pointer[i] == '~') {
                if (i + 1 >= pointer.size() || (pointer[i + 1] != '0' && pointer[i + 1] != '1'))
                    return Error("json.pointer.invalid", "invalid JSON Pointer escape", {},
                                 "use ~0 for ~ or ~1 for /");
                token += pointer[i + 1] == '0' ? '~' : '/';
                i += 2;
            } else
                token += pointer[i++];
        }
        tokens.push_back(std::move(token));
        ++i;
    }
    return tokens;
}
const Value* resolve(const Value& root, std::string_view pointer) {
    auto tokens = pointerSplit(pointer);
    if (!tokens) return nullptr;
    const Value* cur = &root;
    for (const auto& token : *tokens) {
        if (cur->isObject())
            cur = cur->find(token);
        else if (cur->isArray()) {
            if (token.empty() || (token.size() > 1 && token[0] == '0')) return nullptr;
            std::size_t n = 0;
            auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), n);
            if (ec != std::errc{} || p != token.data() + token.size() || n >= cur->size())
                return nullptr;
            cur = &cur->at(n);
        } else
            return nullptr;
        if (!cur) return nullptr;
    }
    return cur;
}
const Value* resolveFragment(const Value& root, std::string_view reference) {
    if (reference.empty() || reference.front() != '#') return nullptr;
    std::string pointer;
    pointer.reserve(reference.size());
    for (std::size_t i = 1; i < reference.size(); ++i) {
        if (reference[i] != '%') {
            pointer += reference[i];
            continue;
        }
        unsigned byte = 0;
        if (reference.size() - i < 3) return nullptr;
        auto [end, ec] =
            std::from_chars(reference.data() + i + 1, reference.data() + i + 3, byte, 16);
        if (ec != std::errc{} || end != reference.data() + i + 3) return nullptr;
        pointer += static_cast<char>(byte);
        i += 2;
    }
    return resolve(root, pointer);
}
}  // namespace supple::json
