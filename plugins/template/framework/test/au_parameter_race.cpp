// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The AU as a host runs it, in this process: rendering on one thread,
// automation (AudioUnitSetParameter) on two more, and state loads
// (ClassInfo) on the main thread, each of which makes the plugin rescan its
// parameters' values. A host may do all of it at once. Under the tsan
// preset, ThreadSanitizer reports any access two of them make to the same
// memory without ordering, whether or not they collide in this run.
//
//   au-parameter-race <Product.component>
//
// The component is registered in this process from its own bundle, so
// nothing is installed and the system's AU cache is not involved.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

OSStatus silence(void*, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32, UInt32,
                 AudioBufferList* io) {
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
        if (io->mBuffers[b].mData) std::memset(io->mBuffers[b].mData, 0, io->mBuffers[b].mDataByteSize);
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
    return noErr;
}

void check(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    std::fflush(stdout);
    if (!ok) ++failures;
}

// "aufx" in the bundle's Info.plist, as the four-character code it names.
OSType fourCC(CFDictionaryRef entry, CFStringRef key) {
    const auto text = static_cast<CFStringRef>(CFDictionaryGetValue(entry, key));
    char c[8] = {};
    if (!text || CFGetTypeID(text) != CFStringGetTypeID() ||
        !CFStringGetCString(text, c, sizeof(c), kCFStringEncodingMacRoman) || std::strlen(c) != 4)
        return 0;
    return (OSType(uint8_t(c[0])) << 24) | (OSType(uint8_t(c[1])) << 16) | (OSType(uint8_t(c[2])) << 8) |
           OSType(uint8_t(c[3]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: au-parameter-race <Product.component>\n");
        return 2;
    }
    std::printf("AU parameter race (%s)\n", argv[1]);

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(argv[1]),
                                                           static_cast<CFIndex>(std::strlen(argv[1])), true);
    CFBundleRef bundle = url ? CFBundleCreate(nullptr, url) : nullptr;
    check(bundle != nullptr, "the bundle opens");
    if (!bundle) return 1;

    const auto components =
        static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
    const auto entry = components && CFGetTypeID(components) == CFArrayGetTypeID() && CFArrayGetCount(components) > 0
                           ? static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(components, 0))
                           : nullptr;
    check(entry != nullptr, "Info.plist lists an audio component");
    if (!entry) return 1;

    AudioComponentDescription desc{};
    desc.componentType = fourCC(entry, CFSTR("type"));
    desc.componentSubType = fourCC(entry, CFSTR("subtype"));
    desc.componentManufacturer = fourCC(entry, CFSTR("manufacturer"));
    const auto factoryName = static_cast<CFStringRef>(CFDictionaryGetValue(entry, CFSTR("factoryFunction")));
    auto factory = factoryName ? reinterpret_cast<AudioComponentFactoryFunction>(
                                     CFBundleGetFunctionPointerForName(bundle, factoryName))
                               : nullptr;
    check(desc.componentType && factory, "its factory function loads");
    if (!factory) return 1;

    AudioComponent component = AudioComponentRegister(&desc, CFSTR("Supple: race probe"), 1, factory);
    AudioComponentInstance unit = nullptr;
    check(component && AudioComponentInstanceNew(component, &unit) == noErr && unit, "an instance is made");
    if (!unit) return 1;
    // Its inputs, a sidechain too, are silence. A note effect has no audio.
    UInt32 inputs = 0, outputs = 0;
    UInt32 size = sizeof(inputs);
    AudioUnitGetProperty(unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputs, &size);
    size = sizeof(outputs);
    AudioUnitGetProperty(unit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &outputs, &size);
    for (UInt32 bus = 0; bus < inputs; ++bus) {
        AURenderCallbackStruct callback{silence, nullptr};
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus, &callback,
                             sizeof(callback));
    }
    check(AudioUnitInitialize(unit) == noErr, "it initialises");
    AudioStreamBasicDescription format{};
    size = sizeof(format);
    AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
    const UInt32 channels = format.mChannelsPerFrame ? format.mChannelsPerFrame : 2;

    // Its parameters.
    size = 0;
    AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr);
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (!ids.empty())
        AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
    check(!ids.empty(), "it lists parameters");
    if (ids.empty()) return 1;
    // Each one's range, to automate it across.
    std::vector<AudioUnitParameterInfo> infos(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        size = sizeof(infos[i]);
        AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, ids[i], &infos[i], &size);
        if (infos[i].cfNameString && (infos[i].flags & kAudioUnitParameterFlag_CFNameRelease))
            CFRelease(infos[i].cfNameString);
    }

    CFPropertyListRef state = nullptr;
    size = sizeof(state);
    check(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state, &size) ==
                  noErr &&
              state,
          "its state saves");
    if (!state) return 1;

    // Rendering and automation, as fast as they go, until the loads are done.
    std::atomic<bool> stop{false};
    std::atomic<long> sets{0}, blocks{0};
    std::atomic<bool> rendered{true};
    std::thread render([&] {
        constexpr UInt32 frames = 256;
        std::vector<float> samples(frames * channels);
        std::vector<uint8_t> listBytes(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * channels);
        auto* list = reinterpret_cast<AudioBufferList*>(listBytes.data());
        AudioTimeStamp time{};
        time.mFlags = kAudioTimeStampSampleTimeValid;
        while (outputs > 0 && !stop.load(std::memory_order_relaxed)) {
            list->mNumberBuffers = channels;
            for (UInt32 c = 0; c < channels; ++c)
                list->mBuffers[c] = {1, static_cast<UInt32>(frames * sizeof(float)), samples.data() + c * frames};
            AudioUnitRenderActionFlags flags = 0;
            if (AudioUnitRender(unit, &flags, &time, 0, frames, list) != noErr) rendered = false;
            time.mSampleTime += frames;
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    auto automate = [&](int phase) {
        for (long n = phase; !stop.load(std::memory_order_relaxed); ++n) {
            for (size_t i = 0; i < ids.size(); ++i)
                AudioUnitSetParameter(unit, ids[i], kAudioUnitScope_Global, 0,
                                      (n & 1) ? infos[i].maxValue : infos[i].minValue, 0);
            sets.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread automation(automate, 0), more(automate, 1);
    // Everything is under way before the first load, so all of it overlaps.
    while (sets.load() < 100 || (outputs > 0 && blocks.load() < 10)) std::this_thread::yield();

    bool loaded = true;
    for (int i = 0; i < 200; ++i) {
        loaded = loaded && AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &state,
                                                sizeof(state)) == noErr;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);  // the wrapper's main-thread work
    }
    stop = true;
    automation.join();
    more.join();
    render.join();
    check(loaded, "200 state loads, each a parameter rescan");
    std::printf("     %ld rounds of automation and %ld blocks alongside them\n", sets.load(), blocks.load());
    check(sets.load() > 100, "automation ran throughout");
    if (outputs > 0) check(rendered.load() && blocks.load() > 10, "it rendered throughout");

    CFRelease(state);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    CFRelease(bundle);
    CFRelease(url);
    std::printf("\n%d failed\n", failures);
    return failures ? 1 : 0;
}
