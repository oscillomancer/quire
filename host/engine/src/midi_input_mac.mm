// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <algorithm>
#include "midi_input.h"
#include "supple/product.h"
namespace supple::engine::detail {
namespace {
class CoreMidiSource final : public MidiSource {
    MIDIClientRef client_ = 0;
    MIDIPortRef input_ = 0;
    MidiInputBuffer* buffer_ = nullptr;
    std::array<MIDIEndpointRef, kMidiInputPorts> sources_{};
    double secondsPerTick_ = 0;
    ItemCount sourceCount_ = 0;
    void receive(const MIDIEventList* list, std::size_t port) SUPPLE_RT {
        const auto* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; ++p) {
            const double stamp = packet->timeStamp
                                     ? static_cast<double>(packet->timeStamp) * secondsPerTick_
                                     : now();
            for (UInt32 i = 0; i < packet->wordCount;) {
                const auto word = packet->words[i];
                const auto type = word >> 28;
                if (type == 2) {  // MIDI 1.0 channel voice UMP, one word
                    const auto message =
                        ((word >> 16) & 0xff) | ((word >> 8) & 0xff) << 8 | (word & 0xff) << 16;
                    buffer_->push(port, stamp, message);
                } else {
                    buffer_->drop();  // system, SysEx, MIDI 2, utility/flex
                }
                // UMP type determines its size, including unsupported messages.
                constexpr unsigned words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
                i += words[type];
            }
            packet = MIDIEventPacketNext(packet);
        }
    }

  public:
    ~CoreMidiSource() override { stop(); }
    Status start(MidiInputBuffer& buffer) override {
        stop();
        buffer_ = &buffer;
        mach_timebase_info_data_t timebase{};
        mach_timebase_info(&timebase);
        secondsPerTick_ = static_cast<double>(timebase.numer) / timebase.denom * 1e-9;
        const auto title =
            CFStringCreateWithCString(nullptr, product::kName.data(), kCFStringEncodingUTF8);
        auto error = MIDIClientCreate(title, nullptr, nullptr, &client_);
        if (!error)
            error = MIDIInputPortCreateWithProtocol(
                client_, title, kMIDIProtocol_1_0, &input_,
                ^(const MIDIEventList* list, void* context) {
                  receive(list, reinterpret_cast<std::uintptr_t>(context));
                });
        CFRelease(title);
        auto fail = [&](OSStatus code) -> Status {
            stop();
            return Error(
                "backend.midi", "CoreMIDI input setup failed (" + std::to_string(code) + ")",
                "/midi_inputs",
                "close other MIDI clients and reconnect the device, then restart the engine");
        };
        if (error) return fail(error);
        sourceCount_ = MIDIGetNumberOfSources();
        const auto count = std::min<ItemCount>(sourceCount_, kMidiInputPorts);
        // Publish all names before connecting: callbacks may start immediately.
        for (ItemCount i = 0; i < count; ++i) {
            sources_[i] = MIDIGetSource(i);
            CFStringRef name = nullptr;
            MIDIObjectGetStringProperty(sources_[i], kMIDIPropertyDisplayName, &name);
            std::string text;
            if (name) {
                const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name),
                                                                        kCFStringEncodingUTF8) +
                                      1;
                text.resize(static_cast<std::size_t>(capacity));
                if (CFStringGetCString(name, text.data(), capacity, kCFStringEncodingUTF8))
                    text.resize(std::char_traits<char>::length(text.c_str()));
                else
                    text.clear();
                CFRelease(name);
            }
            if (text.empty()) text = "MIDI input " + std::to_string(i + 1);
            buffer.names.push_back(std::move(text));
        }
        for (ItemCount i = 0; i < count; ++i) {
            error = MIDIPortConnectSource(input_, sources_[i], reinterpret_cast<void*>(i));
            if (error) return fail(error);
        }
        return okStatus();
    }
    void stop() override {
        if (input_) MIDIPortDispose(input_);
        input_ = 0;
        if (client_) MIDIClientDispose(client_);
        client_ = 0;
        buffer_ = nullptr;
    }
    double now() const SUPPLE_RT override {
        return static_cast<double>(mach_absolute_time()) * secondsPerTick_;
    }
    bool changed() override {
        if (MIDIGetNumberOfSources() != sourceCount_) return true;
        for (std::size_t i = 0; i < buffer_->names.size(); ++i)
            if (MIDIGetSource(i) != sources_[i]) return true;
        return false;
    }
};
}  // namespace
std::unique_ptr<MidiSource> nativeMidiSource() { return std::make_unique<CoreMidiSource>(); }
}  // namespace supple::engine::detail
