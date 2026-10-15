// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/paths.h>
#include <cstdlib>
#include <optional>
#include <string>
#if defined(__APPLE__)
#include <unistd.h>
#include <climits>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
struct Environment {
    std::string key;
    std::optional<std::string> old;
    Environment(const char* name, const char* value) : key(name) {
        if (const char* previous = std::getenv(name)) old = previous;
        set(value);
    }
    void set(const char* value) {
#if defined(_WIN32)
        REQUIRE(_putenv_s(key.c_str(), value ? value : "") == 0);
#else
        REQUIRE((value ? setenv(key.c_str(), value, 1) : unsetenv(key.c_str())) == 0);
#endif
    }
    ~Environment() {
#if defined(_WIN32)
        _putenv_s(key.c_str(), old ? old->c_str() : "");
#else
        if (old)
            setenv(key.c_str(), old->c_str(), 1);
        else
            unsetenv(key.c_str());
#endif
    }
};
}  // namespace
TEST_CASE("platform paths select native user directories without creating them") {
    using namespace supple::paths;
#if defined(_WIN32)
    Environment user("USERPROFILE", "C:/test-user");
    Environment roaming("APPDATA", "C:/roaming");
    Environment local("LOCALAPPDATA", "C:/local");
    CHECK(home() == "C:/test-user");
    CHECK(config() == "C:/roaming");
    CHECK(data() == config());
    CHECK(cache() == "C:/local");
    CHECK(runtime() == "C:/local/Temp");
#else
    Environment user("HOME", "/tmp/test-user");
    Environment configEnv("XDG_CONFIG_HOME", "/tmp/config");
    Environment dataEnv("XDG_DATA_HOME", "/tmp/data");
    Environment cacheEnv("XDG_CACHE_HOME", "/tmp/cache");
    Environment runtimeEnv("XDG_RUNTIME_DIR", "/tmp/runtime");
#if defined(__APPLE__)
    CHECK(config() == "/tmp/test-user/Library/Application Support");
    CHECK(data() == config());
    CHECK(cache() == "/tmp/test-user/Library/Caches");
    // Without TMPDIR: the per-user temporary directory TMPDIR normally names.
    Environment temporary("TMPDIR", nullptr);
    char perUser[PATH_MAX];
    REQUIRE(::confstr(_CS_DARWIN_USER_TEMP_DIR, perUser, sizeof perUser) > 0);
    CHECK(runtime() == std::filesystem::path(perUser));
    CHECK(runtime() != "/tmp");
#else
    CHECK(config() == "/tmp/config");
    CHECK(data() == "/tmp/data");
    CHECK(cache() == "/tmp/cache");
    CHECK(runtime() == "/tmp/runtime");
    configEnv.set(nullptr);
    dataEnv.set("");
    cacheEnv.set(nullptr);
    runtimeEnv.set(nullptr);
    CHECK(config() == "/tmp/test-user/.config");
    CHECK(data() == "/tmp/test-user/.local/share");
    CHECK(cache() == "/tmp/test-user/.cache");
    // Without XDG_RUNTIME_DIR: the login's private /run/user/<uid>, else /tmp.
    const std::string login = "/run/user/" + std::to_string(::getuid());
    struct stat info{};
    const bool loginDir = ::stat(login.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
                          info.st_uid == ::getuid() && (info.st_mode & 077) == 0;
    CHECK(runtime() == (loginDir ? login : std::string("/tmp")));
    user.set("");
    CHECK(cache().empty());
    CHECK(data().empty());
    CHECK(cache(true) == ".cache");
    CHECK(data(true) == ".local/share");
    user.set(nullptr);
    CHECK(cache(true) == "/tmp/.cache");
    CHECK(data(true) == "/tmp/.local/share");
#endif
#endif
}
