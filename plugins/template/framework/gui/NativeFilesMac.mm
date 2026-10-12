// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// macOS: NSOpenPanel and NSSavePanel, begun without blocking; the host's
// run loop drives them. Built without ARC, like pugl_metal.m: the panel is
// retained while it is open and released when it ends or the editor goes.

#include "gui/NativeFiles.h"

#import <Cocoa/Cocoa.h>

#include <memory>

namespace supple::gui {

// What a panel's completion handler reports to, which outlives the editor:
// once the editor is gone, the handler finds `alive` false and does nothing.
struct Outcome {
    bool alive = true;
    bool finished = false;
    std::vector<std::string> paths;
};

struct NativeFiles::Platform {
    NSSavePanel* panel = nil;  // retained; an NSOpenPanel is one
    std::shared_ptr<Outcome> outcome = std::make_shared<Outcome>();

    ~Platform() {
        outcome->alive = false;
        if (panel) {
            [panel cancel:nil];
            [panel release];
        }
    }
};

namespace {
NSString* string(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
}  // namespace

bool NativeFiles::start(const Options& o, bool saving, Done done) {
    if (platform_) return false;
    NSSavePanel* panel = nil;
    if (saving) {
        panel = [NSSavePanel savePanel];
        if (!o.name.empty()) [panel setNameFieldStringValue:string(o.name)];
    } else {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        [open setCanChooseFiles:YES];
        [open setCanChooseDirectories:NO];
        [open setAllowsMultipleSelection:o.multiple ? YES : NO];
        panel = open;
    }
    if (!o.title.empty()) {
        [panel setTitle:string(o.title)];
        [panel setMessage:string(o.title)];
    }
    if (!o.extensions.empty()) {
        NSMutableArray* types = [NSMutableArray array];
        for (const std::string& e : o.extensions) [types addObject:string(e.substr(e[0] == '.' ? 1 : 0))];
        // allowedContentTypes (macOS 11) needs UTTypes for each extension;
        // the extensions themselves still work everywhere this builds.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [panel setAllowedFileTypes:types];
#pragma clang diagnostic pop
    }
    if (!o.folder.empty()) [panel setDirectoryURL:[NSURL fileURLWithPath:string(o.folder) isDirectory:YES]];

    platform_ = std::make_unique<Platform>();
    platform_->panel = [panel retain];
    done_ = std::move(done);
    std::shared_ptr<Outcome> outcome = platform_->outcome;
    const bool opening = !saving;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      if (!outcome->alive) return;
      if (result == NSModalResponseOK) {
          if (opening) {
              for (NSURL* url in [(NSOpenPanel*)panel URLs])
                  if ([url isFileURL]) outcome->paths.emplace_back([[url path] UTF8String]);
          } else if (NSURL* url = [panel URL]) {
              outcome->paths.emplace_back([[url path] UTF8String]);
          }
      }
      outcome->finished = true;
    }];
    return true;
}

void NativeFiles::poll() {
    if (!platform_ || !platform_->outcome->finished) return;
    std::vector<std::string> paths = std::move(platform_->outcome->paths);
    platform_.reset();
    Done done = std::move(done_);
    done_ = nullptr;
    if (done) done(paths);
}

NativeFiles::NativeFiles(uintptr_t parent) : parent_(parent) {}
NativeFiles::~NativeFiles() = default;

}  // namespace supple::gui
