#include <doctest/doctest.h>
#include <sys/mman.h>
#include <unistd.h>
#include <filesystem>

#include <cerrno>

#include "runtime_protection.h"
#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;

namespace {
struct MemoryOps {
    std::shared_ptr<RuntimeProtectionOps> ops = std::make_shared<RuntimeProtectionOps>();
    int locks = 0, unlocks = 0, lockError = 0, unlockError = 0, flags = 0;
    MemoryOps() {
        ops->lockMemory = [this](int f) {
            ++locks;
            flags = f;
            errno = lockError;
            return lockError ? -1 : 0;
        };
        ops->unlockMemory = [this] {
            ++unlocks;
            errno = unlockError;
            return unlockError ? -1 : 0;
        };
        ops->memoryLimit = [](rlimit* limit) {
            limit->rlim_cur = 4096;
            return 0;
        };
    }
};
}  // namespace

TEST_CASE("memory lock ownership, flags, pending intent and repeated lifecycle") {
    MemoryOps fake;
    {
        detail::RuntimeProtection controls(fake.ops);
        controls.start();
        controls.stop();
        CHECK(fake.locks == 0);
        CHECK(fake.unlocks == 0);
        REQUIRE(controls.setMemoryLock(true, false));
        CHECK(controls.memoryLock().requested);
        CHECK_FALSE(controls.memoryLock().active);
        CHECK(fake.locks == 0);
        controls.start();
        CHECK(fake.flags == (MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT));
        CHECK(controls.memoryLock().active);
        REQUIRE(controls.setMemoryLock(true, true));
        CHECK(fake.locks == 1);
        controls.stop();
        controls.stop();
        CHECK(fake.unlocks == 1);
        CHECK(controls.memoryLock().requested);
        controls.start();
        CHECK(fake.locks == 2);
        REQUIRE(controls.setMemoryLock(false, true));
        CHECK_FALSE(controls.memoryLock().active);
        CHECK_FALSE(controls.memoryLock().requested);
        CHECK(fake.unlocks == 2);
        REQUIRE(controls.setMemoryLock(true, true));
    }
    CHECK(fake.unlocks == 3);  // destructor releases the acquired policy
}

TEST_CASE("memory lock failures retain intent and actionable errno, unlock can be retried") {
    MemoryOps fake;
    detail::RuntimeProtection controls(fake.ops);
    for (int value : {ENOMEM, EPERM, EINVAL, EAGAIN}) {
        fake.lockError = value;
        const auto failed = controls.setMemoryLock(true, true);
        REQUIRE_FALSE(failed);
        CHECK(controls.memoryLock().requested);
        CHECK_FALSE(controls.memoryLock().active);
        REQUIRE(controls.memoryLock().error);
        CHECK(controls.memoryLock().error->errnoValue == value);
        CHECK(failed.error().path == "/lock_memory");
        CHECK_FALSE(failed.error().hint.empty());
        if (value == ENOMEM || value == EPERM)
            CHECK(failed.error().hint.find("4096 bytes") != std::string::npos);
        controls.stop();
        CHECK(fake.unlocks == 0);  // failed acquisition must not remove a plug-in's locks
    }
    fake.lockError = 0;
    controls.start();
    REQUIRE(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().error);
    fake.unlockError = EPERM;
    CHECK_FALSE(controls.setMemoryLock(false, true));
    CHECK(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().requested);
    CHECK(controls.memoryLock().error->code == "engine.memory_unlock");
    fake.unlockError = 0;
    controls.stop();
    CHECK_FALSE(controls.memoryLock().active);
    CHECK_FALSE(controls.memoryLock().error);
}

TEST_CASE(
    "EngineApp applies memory policy after start, preserves intent across replacement and stop") {
    MemoryOps fake;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-protection-app-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    {
        AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.templateDir = dir / "templates";
        options.pluginPath = {FIXTURE_CLAP_PATH};
        options.runtimeProtectionOps = fake.ops;
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        auto app = std::move(*created);
        REQUIRE(app->setMemoryLock(true));
        CHECK(app->engineStatus().memoryLock.requested);
        CHECK_FALSE(app->engineStatus().memoryLock.active);
        CHECK(fake.locks == 0);
        CHECK_FALSE(app->startEngine(backend, {.blockSize = 0}));
        CHECK(fake.locks == 0);
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(fake.locks == 1);
        CHECK(app->engineStatus().memoryLock.active);
        REQUIRE(app->transport("stop"));
        CHECK(fake.unlocks == 0);  // transport stop still processes monitored inputs
        REQUIRE(app->newProject({.console9 = false}));
        CHECK(fake.unlocks == 1);
        CHECK(fake.locks == 2);
        app->stopEngine();
        CHECK_FALSE(app->engineStatus().memoryLock.active);
        CHECK(app->engineStatus().memoryLock.requested);
        CHECK(fake.unlocks == 2);
        fake.lockError = ENOMEM;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK(app->engineStatus().running);
        REQUIRE(app->engineStatus().memoryLock.error);
        CHECK(app->engineStatus().memoryLock.error->errnoValue == ENOMEM);
        CHECK_FALSE(app->engineStatus().warnings.empty());
        app->stopEngine();
        CHECK(fake.unlocks == 2);
        CHECK(app->engineStatus().memoryLock.error);
        fake.lockError = 0;
        REQUIRE(app->startEngine(backend, {}, {.anticipative = false, .workers = 0}));
        CHECK_FALSE(app->engineStatus().memoryLock.error);
        // Destructor must stop the backend before releasing the memory policy.
    }
    CHECK(fake.unlocks == 3);
    std::filesystem::remove_all(dir);
}
