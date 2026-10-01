#include "supple/base/sha256.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace supple {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t kInitial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr int kBlock = 64;

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : digest) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0f]);
    }
    return out;
}

}  // namespace

Sha256::Sha256() { std::copy(std::begin(kInitial), std::end(kInitial), state_.begin()); }

void Sha256::block(const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
               (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> bytes) {
    if (finished_) return;
    const std::uint8_t* p = bytes.data();
    std::size_t n = bytes.size();
    bytes_ += n;
    while (n > 0) {
        if (buffered_ == 0 && n >= kBlock) {  // whole blocks straight from the caller
            block(p);
            p += kBlock;
            n -= kBlock;
            continue;
        }
        const auto take = std::min<std::size_t>(n, kBlock - buffered_);
        std::memcpy(buffer_.data() + buffered_, p, take);
        buffered_ += take;
        p += take;
        n -= take;
        if (buffered_ == kBlock) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

void Sha256::update(std::string_view text) {
    update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                         text.size()));
}

std::array<std::uint8_t, 32> Sha256::finish() {
    // Merkle–Damgård padding: 0x80, then zeros until the length lands at byte
    // 56 of a block, then the message length in bits (big endian).
    const std::uint64_t bits = bytes_ * 8;
    const std::size_t zeros = buffered_ < 56 ? 55 - buffered_ : 119 - buffered_;
    std::array<std::uint8_t, 72> pad{};
    pad[0] = 0x80;
    update(std::span<const std::uint8_t>(pad.data(), 1 + zeros));
    for (int i = 0; i < 8; ++i)
        pad[static_cast<std::size_t>(i)] = std::uint8_t(bits >> (56 - 8 * i));
    update(std::span<const std::uint8_t>(pad.data(), 8));
    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i)
        for (int b = 0; b < 4; ++b)
            digest[static_cast<std::size_t>(i * 4 + b)] =
                std::uint8_t(state_[static_cast<std::size_t>(i)] >> (24 - 8 * b));
    finished_ = true;
    return digest;
}

std::string sha256Hex(std::span<const std::uint8_t> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hex(hash.finish());
}

std::string sha256Hex(std::string_view text) {
    Sha256 hash;
    hash.update(text);
    return hex(hash.finish());
}

std::string sha256Hex(const std::array<std::uint8_t, 32>& digest) { return hex(digest); }

Result<std::string> sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error("file.open", "cannot open " + path.string(), path.string(), "check the path");
    Sha256 hash;
    std::array<std::uint8_t, 1 << 16> buffer{};
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        const auto got = in.gcount();
        if (got > 0)
            hash.update(
                std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(got)));
    }
    if (!in.eof()) return Error("file.read", "short read from " + path.string());
    return "sha256:" + hex(hash.finish());
}

}  // namespace supple
