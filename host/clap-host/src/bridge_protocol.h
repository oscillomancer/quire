// Internal: the plugin bridge protocol (ADR-0013), shared by the host side
// (bridge_client.cpp) and the bridge executable (bridge/bridge_main.cpp).
//
// Control plane: a Unix socketpair; messages are [u32 size][u32 op][u32 instance][payload]
// and replies [u32 size][u32 status][u32 instance][payload], little endian, written and
// read whole. File descriptors (the shared memory) ride SCM_RIGHTS.
// Data plane: one memfd per active instance with a Header, the event arenas
// and the audio buffers; each side wakes the other with a futex on a 32-bit
// sequence word in the Header.
#pragma once

#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"

namespace supple::host::bridge {

inline constexpr std::uint32_t kMagic = 0x31474253;  // "SBG1"
inline constexpr std::uint32_t kVersion = 3;  // 3: Create carries the plugin format
inline constexpr int kControlFd = 3;  // the bridge's end of the socketpair
inline constexpr std::uint32_t kMaxChannels = 16;
inline constexpr std::uint32_t kMaxEvents = 2048;
inline constexpr std::uint32_t kMaxOutEvents = 1024;
inline constexpr std::uint32_t kMaxMessage = 64u << 20;

// --- control plane -----------------------------------------------------------------

enum class Op : std::uint32_t {
    Create = 1,  // path, id, identity → info
    Activate,    // rate, min, max (+ memfd) → ok
    Deactivate,
    Params,  // → [ParamInfo]
    ParamValue,
    ParamText,
    ParamFromText,
    SetParam,
    Latency,  // → u32 latency, u32 tail, u8 reportsTail
    SemanticSchema,
    GetPatch,
    SetPatch,
    Describe,
    SaveState,
    LoadState,
    Service,  // serviceMainThread → didWork, notifications, log lines, needsAudioFlush
    Destroy,
};

enum class Reply : std::uint32_t { Ok = 0, Error = 1 };

// Byte-oriented encoding of the payloads.
class Writer {
  public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void f64(double v) { raw(&v, 8); }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void blob(const std::vector<std::uint8_t>& b) {
        u32(static_cast<std::uint32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void error(const Error& e) {
        str(e.code);
        str(e.message);
        str(e.path);
        str(e.hint);
    }

  private:
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
};

class Reader {
  public:
    Reader(const std::uint8_t* data, std::size_t size) : p_(data), end_(data + size) {}
    bool ok() const { return ok_; }
    std::uint8_t u8() {
        std::uint8_t v = 0;
        raw(&v, 1);
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int64_t i64() {
        std::int64_t v = 0;
        raw(&v, 8);
        return v;
    }
    double f64() {
        double v = 0;
        raw(&v, 8);
        return v;
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::vector<std::uint8_t> blob() {
        const std::uint32_t n = u32();
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return {};
        }
        std::vector<std::uint8_t> b(p_, p_ + n);
        p_ += n;
        return b;
    }
    Error error() {
        std::string code = str(), message = str(), path = str(), hint = str();
        return Error(std::move(code), std::move(message), std::move(path), std::move(hint));
    }

  private:
    void raw(void* out, std::size_t n) {
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n) {
            ok_ = false;
            return;
        }
        std::memcpy(out, p_, n);
        p_ += n;
    }
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

// Writes one whole frame ([u32 size][u32 head][payload]) with an optional
// file descriptor; reads one (size-checked). False when the peer is gone.
bool sendFrame(int socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, int fd = -1);
bool recvFrame(int socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, int* fd = nullptr, int timeoutMs = -1);

// --- data plane ---------------------------------------------------------------------

// Commands the host runs on the bridge's audio thread, in order.
enum class Cmd : std::uint32_t { Process = 1, StartProcessing, StopProcessing, Reset, Flush, Quit };

struct Header {
    std::uint32_t magic, version;
    std::uint32_t maxFrames, inChannels, outChannels;
    alignas(64) std::atomic<std::uint32_t> request;  // bumped by the host (futex word)
    alignas(64) std::atomic<std::uint32_t> done;     // = request when finished (futex word)
    alignas(64) std::uint32_t cmd;
    std::uint32_t frames;
    std::int64_t steadyTime;
    std::uint32_t result;  // 1: ok
    std::uint32_t hasTransport;
    Transport transport;
    std::uint32_t eventCount;
    std::uint32_t outEventCount, outEventsDropped;
    std::uint32_t processing;  // the plugin's processing state after the command
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// Offsets of the arenas and buffers in the region.
struct Layout {
    std::size_t events, outEvents, inAudio, outAudio, size;
    std::uint32_t maxFrames, inChannels, outChannels;
    Layout(std::uint32_t frames, std::uint32_t in, std::uint32_t out)
        : maxFrames(frames), inChannels(in), outChannels(out) {
        auto align = [](std::size_t n) { return (n + 63) & ~std::size_t{63}; };
        events = align(sizeof(Header));
        outEvents = align(events + sizeof(Event) * kMaxEvents);
        inAudio = align(outEvents + sizeof(OutputEvent) * kMaxOutEvents);
        outAudio = align(inAudio + sizeof(float) * in * frames);
        size = align(outAudio + sizeof(float) * out * frames);
    }
};

inline long futexWait(std::atomic<std::uint32_t>& word, std::uint32_t expected, int timeoutMs) {
    timespec ts{timeoutMs / 1000, static_cast<long>(timeoutMs % 1000) * 1000000L};
    return ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAIT, expected,
                     timeoutMs < 0 ? nullptr : &ts, nullptr, 0);
}
inline void futexWake(std::atomic<std::uint32_t>& word) {
    ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE, 1, nullptr, nullptr,
              0);
}

}  // namespace supple::host::bridge
