#include <doctest/doctest.h>

#include <pthread.h>

#include <string>
#include <thread>

#include <supple/base/thread.h>

namespace {
std::string nameOf(pthread_t thread) {
    char name[16] = {};
    REQUIRE(::pthread_getname_np(thread, name, sizeof(name)) == 0);
    return name;
}
}  // namespace

TEST_CASE("nameThread names the calling thread and cuts at 15 bytes") {
    std::string shortName, longName;
    std::thread([&] {
        supple::nameThread("audio-test");
        shortName = nameOf(::pthread_self());
        supple::nameThread("audio-a-very-long-name");
        longName = nameOf(::pthread_self());
    }).join();
    CHECK(shortName == "audio-test");
    CHECK(longName == "audio-a-very-lo");
}
