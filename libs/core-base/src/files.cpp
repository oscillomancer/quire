#include "supple/base/files.h"

#include <array>
#include <fstream>

namespace supple {

namespace fs = std::filesystem;

Result<std::vector<std::uint8_t>> readFile(const fs::path& path, std::size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error("file.open", "cannot open " + path.string(), {}, "check the path");
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0) return Error("file.read", "cannot read " + path.string());
    if (static_cast<std::uint64_t>(end) > maxBytes)
        return Error("file.too_large",
                     path.string() + " is larger than " + std::to_string(maxBytes) + " bytes");
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) return Error("file.read", "short read from " + path.string());
    return bytes;
}

Result<std::string> readTextFile(const fs::path& path, std::size_t maxBytes) {
    auto bytes = readFile(path, maxBytes);
    if (!bytes) return std::move(bytes).error();
    return std::string(bytes->begin(), bytes->end());
}

Status writeFileAtomic(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Error("file.write", "cannot write " + tmp.string(), {},
                         "check that the directory exists and is writable");
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out)
            return Error("file.write", "short write to " + tmp.string(), {},
                         "check free disk space");
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return Error("file.write", "cannot move " + tmp.string() + " into place");
    }
    return okStatus();
}

Status writeFileAtomic(const fs::path& path, std::string_view text) {
    return writeFileAtomic(
        path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v =
            (std::uint32_t(bytes[i]) << 16) | (std::uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest) {
        std::uint32_t v = std::uint32_t(bytes[i]) << 16;
        if (rest == 2) v |= std::uint32_t(bytes[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

Result<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    auto bad = [&](std::size_t at) {
        return Error("base64.invalid", "invalid base64 at offset " + std::to_string(at), {},
                     "standard alphabet (A-Z a-z 0-9 + /), '=' padding, no whitespace");
    };
    if (text.size() % 4 != 0) return bad(text.size());
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        std::array<int, 4> d{};
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && last && k >= 2) {
                d[static_cast<std::size_t>(k)] = 0;
                ++pad;
                continue;
            }
            if (pad) return bad(i + static_cast<std::size_t>(k));  // data after padding
            d[static_cast<std::size_t>(k)] = decodeChar(c);
            if (d[static_cast<std::size_t>(k)] < 0) return bad(i + static_cast<std::size_t>(k));
        }
        const std::uint32_t v = (std::uint32_t(d[0]) << 18) | (std::uint32_t(d[1]) << 12) |
                                (std::uint32_t(d[2]) << 6) | std::uint32_t(d[3]);
        // Non-canonical encodings (unused bits set) are rejected too.
        if ((pad == 1 && (v & 0xff)) || (pad == 2 && (v & 0xffff))) return bad(i + 2);
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        if (pad < 2) out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        if (pad < 1) out.push_back(static_cast<std::uint8_t>(v & 0xff));
    }
    return out;
}

}  // namespace supple
