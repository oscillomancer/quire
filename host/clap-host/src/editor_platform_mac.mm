// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#import <AppKit/AppKit.h>
#include <cerrno>
#include <map>
#include <vector>
#include "editor_platform.h"

@interface SuppleEditorDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL closed;
@property(nonatomic) BOOL resized;
@end
@implementation SuppleEditorDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    self.closed = YES;
    return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    self.resized = YES;
}
@end

namespace supple::host {
Status initializeEditorPlatform() { return {}; }
namespace {
void fdReady(CFFileDescriptorRef, CFOptionFlags, void*) {
    // Wake nextEvent after its run loop services an IPC descriptor. No timer poll.
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0]
             atStart:YES];
}
}  // namespace
int waitEditorEvents(std::span<WaitItem> items, int timeoutMs) {
    // Headless bridges do not initialize AppKit or require a display.
    if (!NSApp) return waitFor(items, timeoutMs);
    @autoreleasepool {
        for (int n = 0; n < 64; ++n) {
            NSEvent* pending = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                  untilDate:[NSDate distantPast]
                                                     inMode:NSDefaultRunLoopMode
                                                    dequeue:YES];
            if (!pending) break;
            [NSApp sendEvent:pending];
        }
        [NSApp updateWindows];
        const int ready = waitFor(items, 0);
        if (ready != 0) return ready;
        std::vector<CFFileDescriptorRef> descriptors;
        std::vector<CFRunLoopSourceRef> sources;
        std::map<int, short> watched;
        for (const auto& item : items) watched[item.source.fd()] |= item.events;
        auto cleanup = [&] {
            for (auto source : sources) {
                CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
                CFRelease(source);
            }
            for (auto fd : descriptors) {
                CFFileDescriptorInvalidate(fd);
                CFRelease(fd);
            }
        };
        for (const auto& [fd, events] : watched) {
            auto descriptor =
                CFFileDescriptorCreate(kCFAllocatorDefault, fd, false, fdReady, nullptr);
            if (!descriptor) {
                cleanup();
                errno = ENOMEM;
                return -1;
            }
            descriptors.push_back(descriptor);
            auto source = CFFileDescriptorCreateRunLoopSource(kCFAllocatorDefault, descriptor, 0);
            if (!source) {
                cleanup();
                errno = ENOMEM;
                return -1;
            }
            sources.push_back(source);
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
            CFFileDescriptorEnableCallBacks(
                descriptor, ((events & kWaitRead) ? kCFFileDescriptorReadCallBack : 0) |
                                ((events & kWaitWrite) ? kCFFileDescriptorWriteCallBack : 0));
        }
        NSDate* until =
            timeoutMs < 0
                ? [NSDate distantFuture]
                : [NSDate dateWithTimeIntervalSinceNow:static_cast<double>(timeoutMs) / 1000.0];
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event) [NSApp sendEvent:event];
        [NSApp updateWindows];
        cleanup();
        return waitFor(items, 0);
    }
}
struct FloatingEditorWindow::State {
    NSWindow* window = nil;
    std::uint32_t ownerWindowNumber = 0;
    SuppleEditorDelegate* delegate = nil;
};
FloatingEditorWindow::FloatingEditorWindow() : state_(std::make_unique<State>()) {}
Result<std::unique_ptr<FloatingEditorWindow>> FloatingEditorWindow::create(std::string_view title) {
    @autoreleasepool {
        if (!NSApp) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            [NSApp finishLaunching];
        }
        auto result = std::unique_ptr<FloatingEditorWindow>(new FloatingEditorWindow);
        auto& s = *result->state_;
        s.window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 640, 400)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!s.window)
            return Error("host.editor.window", "cannot create the bridge editor window", "/editor",
                         "open an interactive desktop session and retry");
        s.window.releasedWhenClosed = NO;
        s.window.title = [[NSString alloc] initWithBytes:title.data()
                                                  length:title.size()
                                                encoding:NSUTF8StringEncoding];
        s.window.level = NSNormalWindowLevel;
        s.delegate = [SuppleEditorDelegate new];
        s.window.delegate = s.delegate;
        [s.window center];
        return result;
    }
}
FloatingEditorWindow::~FloatingEditorWindow() {
    @autoreleasepool {
        state_->window.delegate = nil;
        [state_->window close];
    }
}
NativeWindow FloatingEditorWindow::parent() const {
    return {NativeWindow::Kind::Cocoa,
            reinterpret_cast<std::uintptr_t>((__bridge void*)state_->window.contentView)};
}
void FloatingEditorWindow::resize(unsigned width, unsigned height, bool resizable) {
    auto style = state_->window.styleMask;
    if (resizable)
        style |= NSWindowStyleMaskResizable;
    else
        style &= ~NSWindowStyleMaskResizable;
    state_->window.styleMask = style;
    [state_->window setContentSize:NSMakeSize(width, height)];
    state_->delegate.resized = NO;
}
bool FloatingEditorWindow::takeResize(unsigned& width, unsigned& height) {
    if (!state_->delegate.resized) return false;
    state_->delegate.resized = NO;
    const auto size = state_->window.contentView.bounds.size;
    width = static_cast<unsigned>(size.width);
    height = static_cast<unsigned>(size.height);
    return true;
}
void FloatingEditorWindow::show() {
    // Order within the normal level, without activating the bridge or making
    // the editor key. The user can activate it by clicking its native window.
    [state_->window orderWindow:NSWindowAbove relativeTo:state_->ownerWindowNumber];
}
void FloatingEditorWindow::showAbove(std::uint32_t ownerWindowNumber) {
    state_->ownerWindowNumber = ownerWindowNumber;
    show();
}
void FloatingEditorWindow::hide() { [state_->window orderOut:nil]; }
bool FloatingEditorWindow::closed() const { return state_->delegate.closed; }
}  // namespace supple::host
