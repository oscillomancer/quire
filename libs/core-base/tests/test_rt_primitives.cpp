#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <supple/base/handoff.h>
#include <supple/base/rng.h>
#include <supple/base/spsc.h>

using supple::Rng;
using supple::RtHandoff;
using supple::SpscRing;

TEST_CASE("SpscRing: full ring drops the newest item, peek/consume retries") {
    SpscRing<int, 4> ring;
    CHECK(ring.empty());
    CHECK(ring.free() == 4);
    for (int i = 0; i < 4; ++i) CHECK(ring.push(i));
    CHECK_FALSE(ring.push(99));  // dropped, not overwritten
    CHECK(ring.free() == 0);
    int v = -1;
    REQUIRE(ring.peek(v));
    CHECK(v == 0);
    REQUIRE(ring.peek(v));  // peek twice: same item until consumed
    CHECK(v == 0);
    ring.consume();
    REQUIRE(ring.pop(v));
    CHECK(v == 1);
    CHECK(ring.push(4));
    CHECK(ring.push(5));
    std::vector<int> rest;
    while (ring.pop(v)) rest.push_back(v);
    CHECK(rest == std::vector<int>{2, 3, 4, 5});
    CHECK_FALSE(ring.pop(v));
}

TEST_CASE("SpscRing: two threads, one million items, strict FIFO") {
    SpscRing<std::uint64_t, 1024> ring;
    constexpr std::uint64_t kCount = 1'000'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            if (ring.push(i)) ++i;
        }
    });
    std::uint64_t expected = 0;
    bool inOrder = true;
    while (expected < kCount) {
        std::uint64_t v;
        if (ring.pop(v)) {
            inOrder = inOrder && v == expected;
            ++expected;
        }
    }
    producer.join();
    CHECK(inOrder);
    CHECK(ring.empty());
}

namespace {
struct Payload {
    std::uint64_t generation;
    std::uint64_t check;  // generation * 3 + 1, to detect torn or stale reads
    static std::atomic<int> live;
    Payload(std::uint64_t g) : generation(g), check(g * 3 + 1) { ++live; }
    ~Payload() { --live; }
};
std::atomic<int> Payload::live{0};
}  // namespace

TEST_CASE("RtHandoff: single-threaded adoption, replacement and reclamation") {
    {
        RtHandoff<Payload, 2> h;
        CHECK(h.acquire() == nullptr);
        CHECK(h.publish(std::make_unique<Payload>(1)) == 1);
        CHECK(h.publish(std::make_unique<Payload>(2)) == 2);  // replaces unadopted #1
        CHECK(Payload::live == 1);
        Payload* p = h.acquire();
        REQUIRE(p);
        CHECK(p->generation == 2);
        CHECK(h.adoptedGeneration() == 2);
        h.publish(std::make_unique<Payload>(3));
        CHECK(h.acquire()->generation == 3);      // #2 retired
        h.publish(std::make_unique<Payload>(4));  // publish() also collects retired #2
        CHECK(Payload::live == 2);                // #3 in use, #4 pending
        CHECK(h.acquire()->generation == 4);      // #3 retired
        h.collect();
        CHECK(Payload::live == 1);
        h.publish(std::make_unique<Payload>(5));
        CHECK(h.acquire()->generation == 5);
        CHECK(h.current()->generation == 5);
    }
    CHECK(Payload::live == 0);  // destructor frees current, pending and retired
}

TEST_CASE("RtHandoff: concurrent publish/acquire sees monotonic, intact objects") {
    {
        RtHandoff<Payload> h;
        constexpr std::uint64_t kPublications = 20'000;
        std::atomic<bool> done{false};
        std::atomic<bool> ok{true};
        std::thread audio([&] {
            std::uint64_t last = 0;
            while (!done.load(std::memory_order_acquire)) {
                if (Payload* p = h.acquire()) {
                    if (p->check != p->generation * 3 + 1 || p->generation < last) ok = false;
                    last = p->generation;
                }
            }
            if (Payload* p = h.acquire()) last = p->generation;
        });
        for (std::uint64_t g = 1; g <= kPublications; ++g) {
            h.publish(std::make_unique<Payload>(g));
            if (g % 64 == 0) h.collect();
        }
        // Wait until the audio thread has adopted the final publication.
        while (h.adoptedGeneration() != kPublications) h.collect();
        done.store(true, std::memory_order_release);
        audio.join();
        CHECK(ok.load());
        h.collect();
        CHECK(Payload::live <= 1);  // only the current object remains
    }
    CHECK(Payload::live == 0);
}

TEST_CASE("Rng: known-answer values (independent reference implementation)") {
    Rng a(0);
    CHECK(a.next() == 0x99ec5f36cb75f2b4ULL);
    CHECK(a.next() == 0xbf6e1f784956452aULL);
    CHECK(a.next() == 0x1a5f849d4933e6e0ULL);
    CHECK(a.next() == 0x6aa594f1262d2d2cULL);
    Rng b(12345);
    CHECK(b.next() == 0xbe6a36374160d49bULL);
    CHECK(b.next() == 0x214aaa0637a688c6ULL);
    CHECK(b.next() == 0xf69d16de9954d388ULL);
    CHECK(b.next() == 0x0c60048c4e96e033ULL);
}

TEST_CASE("Rng: ranges, reproducibility and independent jumped streams") {
    Rng r(7);
    for (int i = 0; i < 10000; ++i) {
        const double u = r.uniform();
        CHECK((u >= 0.0 && u < 1.0));
        const float f = r.bipolar();
        CHECK((f >= -1.0f && f < 1.0f));
        CHECK(r.below(10) < 10);
    }
    Rng x(42), y(42);
    for (int i = 0; i < 100; ++i) CHECK(x.next() == y.next());
    Rng j(42);
    j.jump();
    Rng k(42);
    int equal = 0;
    for (int i = 0; i < 100; ++i) equal += j.next() == k.next();
    CHECK(equal == 0);
    // below() is unbiased enough that a 3-way split of 30000 draws is near 10000 each.
    Rng c(1);
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 30000; ++i) ++counts[c.below(3)];
    for (int n : counts) CHECK((n > 9500 && n < 10500));
}

// Exercised under the rtsan preset: any allocation, lock or syscall reached
// from these calls aborts the test.
namespace {
int rtWork(SpscRing<int, 8>& ring, RtHandoff<Payload>& handoff, Rng& rng) SUPPLE_RT {
    int sum = 0;
    ring.push(1);
    int v = 0;
    if (ring.pop(v)) sum += v;
    if (Payload* p = handoff.acquire()) sum += static_cast<int>(p->generation);
    sum += static_cast<int>(rng.below(4));
    return sum;
}
}  // namespace

TEST_CASE("RT primitives are real-time safe" * doctest::description("rt")) {
    SpscRing<int, 8> ring;
    RtHandoff<Payload> handoff;
    Rng rng(3);
    handoff.publish(std::make_unique<Payload>(1));
    int total = 0;
    for (int i = 0; i < 100; ++i) total += rtWork(ring, handoff, rng);
    CHECK(total > 0);
}
