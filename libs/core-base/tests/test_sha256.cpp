// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// SHA-256 against the FIPS 180-4 / NIST vectors, plus the incremental and
// file paths the media model uses (P5.3c/D-?): a digest must not depend on how
// the bytes were fed.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "supple/base/files.h"
#include "supple/base/sha256.h"

using namespace supple;

namespace {

constexpr const char* kEmpty = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr const char* kAbc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr const char* k448 = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
constexpr const char* k896 = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1";
constexpr const char* kMillionA =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

std::string at(const char* text) { return sha256Hex(std::string_view(text)); }

}  // namespace

TEST_CASE("sha256: the FIPS 180-4 vectors") {
    CHECK(at("") == kEmpty);
    CHECK(at("abc") == kAbc);
    CHECK(at("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == k448);
    CHECK(at("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopq"
             "klmnopqrlmnopqrsmnopqrstnopqrstu") == k896);
    // A million 'a': the classic long-message vector, fed in one call.
    const std::string million(1000000, 'a');
    CHECK(sha256Hex(million) == kMillionA);
    // The same in 999-byte pieces, and byte by byte: the digest is a function of
    // the bytes, not of the feeding pattern.
    Sha256 chunked;
    for (std::size_t off = 0; off < million.size(); off += 999)
        chunked.update(std::string_view(million).substr(off, 999));
    CHECK(sha256Hex(chunked.finish()) == kMillionA);
    Sha256 single;
    for (char c : million) single.update(std::string_view(&c, 1));
    CHECK(sha256Hex(single.finish()) == kMillionA);
}

TEST_CASE("sha256: boundaries around the block and length padding") {
    // 55, 56, 57, 63, 64, 65 bytes are where padding and the length field move
    // between one and two final blocks.
    const std::string pattern = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (std::size_t n : {0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u, 121u, 128u}) {
        const std::string text = pattern.substr(0, std::min(n, pattern.size()));
        const std::string oneShot = sha256Hex(text);
        Sha256 fed;
        for (char c : text) fed.update(std::string_view(&c, 1));  // one byte at a time
        CHECK(sha256Hex(fed.finish()) == oneShot);
        CHECK(oneShot.size() == 64);
    }
    CHECK(sha256Hex("") == kEmpty);  // one empty update and none at all agree
    Sha256 empty;
    CHECK(sha256Hex(empty.finish()) == kEmpty);
}

TEST_CASE("sha256File hashes the file's bytes, however large") {
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-sha256-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // Small: the bytes of "abc" on disk hash to the "abc" vector.
    REQUIRE(writeFileAtomic(dir / "abc", std::string_view("abc")).ok());
    auto small = sha256File(dir / "abc");
    REQUIRE(small.ok());
    CHECK(*small == std::string("sha256:") + kAbc);

    // Larger than one read buffer (64 KiB), with a byte pattern that repeats.
    std::vector<std::uint8_t> big(1u << 20);  // 1 MiB
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<std::uint8_t>((i * 31 + (i >> 8)) & 0xff);
    REQUIRE(writeFileAtomic(dir / "big", big).ok());
    auto hashed = sha256File(dir / "big");
    REQUIRE(hashed.ok());
    CHECK(*hashed == "sha256:" + sha256Hex(big));
    CHECK(hashed->size() == 7 + 64);  // "sha256:" + 64 lowercase hex digits
    for (char c : hashed->substr(7)) {
        const bool isLowerHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(isLowerHex);
    }

    // A missing file is a structured error, not a digest of nothing.
    auto missing = sha256File(dir / "no-such-file");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == "file.open");
    std::filesystem::remove_all(dir);
}
