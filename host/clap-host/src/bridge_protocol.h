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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"

namespace supple::host::bridge {

inline constexpr std::uint32_t kMagic = 0x31474253;  // "SBG1"
// 6 (unreleased): error details, restart failures, complete output event
// metadata in shared memory, an idle-output drain, process wake and ordered clears.
inline constexpr std::uint32_t kVersion = 7;
inline constexpr int kControlFd = 3;  // the bridge's end of the socketpair
inline constexpr std::uint32_t kMaxChannels = 16;
inline constexpr std::uint32_t kMaxEvents = 2048;
inline constexpr std::uint32_t kMaxOutEvents = 1024;
inline constexpr std::uint32_t kMaxParamClears = 4096;
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
    // The two parameter domains a VST3 has (identity for CLAP): main-thread
    // conversions for a host that holds a value in the other domain. Appended
    // so the numbers of the ops above do not change.
    NormalizedParam,  // id, plain → normalized
    PlainParam,       // id, normalized → plain
    // The plug-in's editor, embedded by the bridge into the host's X11 window
    // (window ids are the X server's, not a process's).
    EditorOpen,       // u64 parent window, f64 scale → u32 width, u32 height, u8 resizable
    EditorResize,     // u32 width, u32 height → the accepted size (as EditorOpen)
    EditorClose,      // → ok
    EditorRequests,   // → u8 resize, u8 closed, u32 width, u32 height, u8 show, u8 hide
    TakeIdleOutputs,  // → u64 dropped, u32 count, [OutputEvent]
};

enum class Reply : std::uint32_t { Ok = 0, Error = 1 };

// Byte-oriented encoding of the payloads.
class Writer {
  public:
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void i16(std::int16_t v) { raw(&v, 2); }
    void i32(std::int32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
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
        u32(static_cast<std::uint32_t>(e.details.size()));
        for (const auto& detail : e.details) error(detail);
    }

  private:
    void raw(const void* p, std::size_t n) {
        if (!n) return;
        const auto* b = static_cast<const std::uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
};

class Reader {
  public:
    Reader(const std::uint8_t* data, std::size_t size)
        : p_(size ? data : &empty_), end_(p_ + size) {}
    bool ok() const { return ok_; }
    bool done() const { return ok_ && p_ == end_; }
    void skipString() {
        const auto n = u32();
        skip(n);
    }
    void skip(std::size_t n) {
        if (!ok_ || static_cast<std::size_t>(end_ - p_) < n)
            ok_ = false;
        else
            p_ += n;
    }
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
    std::int16_t i16() {
        std::int16_t v = 0;
        raw(&v, 2);
        return v;
    }
    std::int32_t i32() {
        std::int32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::int64_t i64() {
        std::int64_t v = 0;
        raw(&v, 8);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
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
    Error error(unsigned depth = 0) {
        if (depth >= 32) {
            ok_ = false;
            return {};
        }
        std::string code = str(), message = str(), path = str(), hint = str();
        Error result(std::move(code), std::move(message), std::move(path), std::move(hint));
        const auto count = u32();
        if (count > static_cast<std::size_t>(end_ - p_) / 20) {
            ok_ = false;
            return result;
        }
        for (std::uint32_t i = 0; i < count && ok_; ++i) result.details.push_back(error(depth + 1));
        return result;
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
    inline static constexpr std::uint8_t empty_ = 0;
    const std::uint8_t* p_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

void writeOutputEvent(Writer& writer, const OutputEvent& event);
OutputEvent readOutputEvent(Reader& reader);

using Deadline = std::chrono::steady_clock::time_point;
inline Deadline deadlineAfter(int timeoutMs) {
    return timeoutMs < 0 ? Deadline::max()
                         : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
}
bool validRequest(Op op, Reader r);
bool validReply(Op op, Reader r);

// Writes one whole frame ([u32 size][u32 head][u32 instance][payload]) with an optional
// file descriptor; reads one (size-checked). False when the peer is gone.
bool sendFrame(int socket, std::uint32_t head, std::uint32_t instance,
               const std::vector<std::uint8_t>& payload, int fd = -1,
               Deadline deadline = Deadline::max());
bool recvFrame(int socket, std::uint32_t& head, std::uint32_t& instance,
               std::vector<std::uint8_t>& payload, int* fd = nullptr, int timeoutMs = -1);
bool recvFrameUntil(int socket, std::uint32_t& head, std::uint32_t& instance,
                    std::vector<std::uint8_t>& payload, int* fd, Deadline deadline);

// --- data plane ---------------------------------------------------------------------

// Both endpoints use this native, pointer-free ABI; Layout accounts for its
// v6 size. Control-plane idle events encode each field without ABI padding.
static_assert(std::is_trivially_copyable_v<OutputEvent>);
static_assert(std::is_standard_layout_v<OutputEvent>);
static_assert(sizeof(OutputEvent) == 40);

// Commands the host runs on the bridge's audio thread, in order.
enum class Cmd : std::uint32_t { Process = 1, StartProcessing, StopProcessing, Reset, Flush, Quit };

struct Header {
    std::uint32_t magic, version;
    std::uint32_t maxFrames, inChannels, outChannels;
    std::uint32_t keyChannels, keyActive;            // the sidechain input (ADR-0018)
    alignas(64) std::atomic<std::uint32_t> request;  // bumped by the host (futex word)
    alignas(64) std::atomic<std::uint32_t> done;     // = request when finished (futex word)
    alignas(64) std::uint32_t cmd;
    std::uint32_t frames;
    std::int64_t steadyTime;
    std::uint32_t result;  // 1: ok
    std::uint32_t hasTransport;
    std::uint32_t hasKey;  // this block carries a key (else the plug-in gets silence)
    Transport transport;
    std::uint32_t eventCount;
    std::uint32_t outEventCount, outEventsDropped;
    std::uint32_t processing;  // the plugin's processing state after the command
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

// Offsets of the arenas and buffers in the region.
struct Layout {
    std::size_t events, outEvents, inAudio, keyAudio, outAudio, size;
    std::uint32_t maxFrames, inChannels, outChannels, keyChannels;
    Layout(std::uint32_t frames, std::uint32_t in, std::uint32_t out, std::uint32_t key = 0)
        : maxFrames(frames), inChannels(in), outChannels(out), keyChannels(key) {
        auto align = [](std::size_t n) { return (n + 63) & ~std::size_t{63}; };
        events = align(sizeof(Header));
        outEvents = align(events + sizeof(Event) * kMaxEvents);
        inAudio = align(outEvents + sizeof(OutputEvent) * kMaxOutEvents);
        keyAudio = align(inAudio + sizeof(float) * in * frames);
        outAudio = align(keyAudio + sizeof(float) * key * frames);
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
