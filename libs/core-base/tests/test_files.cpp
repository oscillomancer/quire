#include <doctest/doctest.h>
#include <supple/base/files.h>

#include <unistd.h>
#include <filesystem>
#include <string>

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
    fs::remove_all(dir);
}
