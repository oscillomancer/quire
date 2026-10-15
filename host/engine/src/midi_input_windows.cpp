// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// WinMM declarations require the Windows base types; keep these groups ordered.
#include <mmsystem.h>

#include <algorithm>
#include "midi_input.h"
namespace supple::engine::detail {
namespace {
class WinMidiSource final : public MidiSource {
    struct Port {
        WinMidiSource* owner = nullptr;
        HMIDIIN handle = nullptr;
        unsigned index = 0;
        double anchor = 0;
        MIDIINCAPSW caps{};
        // SysEx buffers are only recycled on the monitor, never in MidiInProc.
        struct LongBuffer {
            MIDIHDR header{};
            std::array<char, 256> bytes{};
            std::atomic<bool> ready{false};
            bool prepared = false;
        };
        std::array<LongBuffer, 2> longBuffers;
    };
    std::array<Port, kMidiInputPorts> ports_;
    MidiInputBuffer* buffer_ = nullptr;
    unsigned deviceCount_ = 0, count_ = 0;
    double frequency_ = 1;
    static void CALLBACK receive(HMIDIIN, UINT message, DWORD_PTR context, DWORD_PTR data,
                                 DWORD_PTR stamp) SUPPLE_RT {
        auto& port = *reinterpret_cast<Port*>(context);
        auto& self = *port.owner;
        if (message == MIM_DATA || message == MIM_MOREDATA) {
            self.buffer_->push(
                port.index,
                winMidiSeconds(static_cast<std::uint32_t>(stamp), port.anchor, self.now()),
                static_cast<std::uint32_t>(data));
        } else if (message == MIM_LONGDATA || message == MIM_LONGERROR) {
            auto* header = reinterpret_cast<MIDIHDR*>(data);
            if (header->dwBytesRecorded) self.buffer_->drop();
            for (auto& item : port.longBuffers)
                if (&item.header == header) item.ready.store(true, std::memory_order_release);
        } else if (message == MIM_ERROR) {
            self.buffer_->drop();
        }
    }

  public:
    ~WinMidiSource() override { stop(); }
    Status start(MidiInputBuffer& buffer) override {
        stop();
        buffer_ = &buffer;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        frequency_ = static_cast<double>(frequency.QuadPart);
        deviceCount_ = midiInGetNumDevs();
        count_ = std::min<unsigned>(deviceCount_, kMidiInputPorts);
        auto fail = [&](MMRESULT error) -> Status {
            stop();
            return Error(
                "backend.midi", "WinMM MIDI input setup failed (" + std::to_string(error) + ")",
                "/midi_inputs",
                "close other MIDI clients and reconnect the device, then restart the engine");
        };
        // Enumerate before opening any callbacks; names/count never mutate live.
        for (unsigned i = 0; i < count_; ++i) {
            auto& port = ports_[i];
            port.owner = this;
            port.index = i;
            auto error = midiInGetDevCapsW(i, &port.caps, sizeof(port.caps));
            if (error) return fail(error);
            const int size = WideCharToMultiByte(CP_UTF8, 0, port.caps.szPname, -1, nullptr, 0,
                                                 nullptr, nullptr);
            std::string name(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, port.caps.szPname, -1, name.data(), size, nullptr,
                                nullptr);
            if (!name.empty()) name.pop_back();
            buffer.names.push_back(std::move(name));
        }
        for (unsigned i = 0; i < count_; ++i) {
            auto& port = ports_[i];
            auto error =
                midiInOpen(&port.handle, i, reinterpret_cast<DWORD_PTR>(&receive),
                           reinterpret_cast<DWORD_PTR>(&port), CALLBACK_FUNCTION | MIDI_IO_STATUS);
            if (error) return fail(error);
            for (auto& item : port.longBuffers) {
                item.ready.store(false);
                item.header = {};
                item.header.lpData = item.bytes.data();
                item.header.dwBufferLength = static_cast<DWORD>(item.bytes.size());
                error = midiInPrepareHeader(port.handle, &item.header, sizeof(MIDIHDR));
                if (error) return fail(error);
                item.prepared = true;
                error = midiInAddBuffer(port.handle, &item.header, sizeof(MIDIHDR));
                if (error) return fail(error);
            }
            port.anchor = now();
            error = midiInStart(port.handle);
            if (error) return fail(error);
        }
        return okStatus();
    }
    void stop() override {
        for (auto& port : ports_) {
            if (!port.handle) continue;
            midiInStop(port.handle);
            midiInReset(port.handle);
            for (auto& item : port.longBuffers) {
                if (item.prepared)
                    midiInUnprepareHeader(port.handle, &item.header, sizeof(MIDIHDR));
                item.prepared = false;
            }
            midiInClose(port.handle);
            port.handle = nullptr;
        }
        count_ = 0;
        buffer_ = nullptr;
    }
    double now() const SUPPLE_RT override {
        LARGE_INTEGER ticks;
        QueryPerformanceCounter(&ticks);
        return static_cast<double>(ticks.QuadPart) / frequency_;
    }
    bool changed() override {
        if (midiInGetNumDevs() != deviceCount_) return true;
        for (unsigned i = 0; i < count_; ++i) {
            auto& port = ports_[i];
            MIDIINCAPSW caps{};
            if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR ||
                caps.wMid != port.caps.wMid || caps.wPid != port.caps.wPid ||
                std::wstring_view(caps.szPname) != port.caps.szPname)
                return true;
            for (auto& item : port.longBuffers)
                if (item.ready.exchange(false, std::memory_order_acq_rel) &&
                    midiInAddBuffer(port.handle, &item.header, sizeof(MIDIHDR)) != MMSYSERR_NOERROR)
                    return true;
        }
        return false;
    }
};
}  // namespace
std::unique_ptr<MidiSource> nativeMidiSource() { return std::make_unique<WinMidiSource>(); }
}  // namespace supple::engine::detail
