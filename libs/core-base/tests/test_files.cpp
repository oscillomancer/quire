// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/files.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace supple;

namespace {
std::vector<std::uint8_t> bytesOf(std::string_view s) { return {s.begin(), s.end()}; }
}  // namespace

TEST_CASE("base64: RFC 4648 test vectors round-trip") {
    const std::pair<const char*, const char*> vectors[] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, encoded] : vectors) {
        const auto b = bytesOf(plain);
        CHECK(base64Encode(b) == encoded);
        auto d = base64Decode(encoded);
        REQUIRE(d.ok());
        CHECK(*d == b);
    }
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    auto d = base64Decode(base64Encode(all));
    REQUIRE(d.ok());
    CHECK(*d == all);
}

TEST_CASE("base64: rejects malformed input") {
    for (const char* bad : {"Zg=", "Zg=a", "Z===", "Zm9v\n", "Zm-v", "Zh==", "Zm9=Zm9v"}) {
        INFO(bad);
        auto d = base64Decode(bad);
        REQUIRE(!d.ok());
        CHECK(d.error().code == "base64.invalid");
    }
}

TEST_CASE("files: atomic write, read back, errors") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-files-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const fs::path p = dir / "sub" / "a.txt";
    REQUIRE(writeFileAtomic(p, std::string_view("hello")).ok());
    CHECK(!fs::exists(p.string() + ".tmp"));
    auto t = readTextFile(p);
    REQUIRE(t.ok());
    CHECK(*t == "hello");
    REQUIRE(writeFileAtomic(p, std::string_view("")).ok());
    CHECK(readTextFile(p).value() == "");
    CHECK(readFile(p, 0).ok());
    REQUIRE(writeFileAtomic(p, std::string_view("12345")).ok());
    CHECK(readFile(p, 4).error().code == "file.too_large");
    CHECK(readFile(dir / "missing").error().code == "file.open");
    // Concurrent writers of one target: every write succeeds, the result is
    // one writer's complete content, and no temporary files are left.
    {
        std::vector<std::thread> ts;
        std::atomic<int> failures{0};
        for (int w = 0; w < 4; ++w)
            ts.emplace_back([&, w] {
                const std::string body(1000, static_cast<char>('a' + w));
                for (int i = 0; i < 50; ++i)
                    if (!writeFileAtomic(p, std::string_view(body)).ok()) ++failures;
            });
        for (auto& th : ts) th.join();
        CHECK(failures == 0);
        const std::string got = readTextFile(p).value();
        REQUIRE(got.size() == 1000);
        CHECK(got == std::string(1000, got[0]));
        int files = 0;
        for (const auto& e : fs::directory_iterator(p.parent_path())) {
            (void)e;
            ++files;
        }
        CHECK(files == 1);
    }
    CHECK(
        writeFileAtomic(dir / "sub" / "a.txt" / "not-a-dir", std::string_view("x")).error().code ==
        "file.write");
    fs::remove_all(dir);
}

TEST_CASE("files: native Unicode paths and binary bytes survive replacement") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-native-" + std::to_string(::getpid()));
    const fs::path path = dir / fs::path(std::u8string(u8"音声.wav"));
    const std::vector<std::uint8_t> first{0, 10, 13, 26, 255};
    const std::vector<std::uint8_t> second{26, 13, 10, 0};
    REQUIRE(writeFileAtomic(path, first).ok());
    REQUIRE(readFile(path).ok());
    CHECK(*readFile(path) == first);
    REQUIRE(writeFileAtomic(path, second).ok());
    REQUIRE(readFile(path).ok());
    CHECK(*readFile(path) == second);
    fs::remove_all(dir);
}

TEST_CASE("files: atomic replacement preserves an existing reader") {
    const fs::path dir =
        fs::temp_directory_path() / ("supple-reader-" + std::to_string(::getpid()));
    const fs::path path = dir / "data";
    REQUIRE(writeFileAtomic(path, std::string_view("old")).ok());
    {
#ifdef _WIN32
        struct Reader {
            HANDLE handle;
            ~Reader() { ::CloseHandle(handle); }
        } reader{::CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        REQUIRE(reader.handle != INVALID_HANDLE_VALUE);
        const auto replaced = writeFileAtomic(path, std::string_view("new"));
        INFO(replaced ? "ok" : replaced.error().message);
        REQUIRE(replaced.ok());
        char old[3];
        DWORD count = 0;
        REQUIRE(::ReadFile(reader.handle, old, sizeof(old), &count, nullptr));
        const std::string previous(old, count);
#else
        std::ifstream reader(path, std::ios::binary);
        REQUIRE(reader.good());
        REQUIRE(writeFileAtomic(path, std::string_view("new")).ok());
        const std::string previous((std::istreambuf_iterator<char>(reader)),
                                   std::istreambuf_iterator<char>());
#endif
        CHECK(previous == "old");
        CHECK(readTextFile(path).value() == "new");
    }
    fs::remove_all(dir);
}
