// UTF-8 validity (RFC 3629: no overlong forms, no surrogates, at most
// U+10FFFF), shared by the parser, the writer and callers that check text
// before it becomes a JSON string.
#pragma once
#include <cstddef>
#include <string_view>
namespace supple::json {
// The length (1..4) of the valid UTF-8 sequence starting at s[i], or 0 when
// the bytes there do not start one.
inline std::size_t utf8SequenceLength(std::string_view s, std::size_t i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    const unsigned count = c >= 0xf0 && c <= 0xf4   ? 4
                           : c >= 0xe0 && c <= 0xef ? 3
                           : c >= 0xc2 && c <= 0xdf ? 2
                                                    : 0;
    if (!count || i + count > s.size()) return 0;
    unsigned cp = c & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
    for (unsigned j = 1; j < count; ++j) {
        const auto d = static_cast<unsigned char>(s[i + j]);
        if ((d & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3f);
    }
    const unsigned least = count == 2 ? 0x80u : count == 3 ? 0x800u : 0x10000u;
    if (cp < least || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
    return count;
}
// The offset of the first byte that starts an invalid sequence, or npos.
inline std::size_t firstInvalidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8SequenceLength(s, i);
        if (!n) return i;
        i += n;
    }
    return std::string_view::npos;
}
}  // namespace supple::json
