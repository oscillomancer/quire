// A track's right-click menu, shared by the TCP and the mixer: Rename, Color…
// (the swatch menu), Timebase… (channels), Channels… (ADR-0020: the main pair
// and auxiliary channels 3-8) and Delete. Each pick is one command (one undo step).
#pragma once

#include <functional>
#include <string>

#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/ui/root.h"

namespace supple::gui {

struct TrackMenu {
    ui::Root& root;
    cmd::Dispatcher& dispatcher;
    engine::EngineApp& app;
    std::function<void(const std::string&)> onError;  // a refused command (status line)

    // `rename` starts the view's own rename (the TCP edits in place); empty:
    // a small rename field opens at the anchor (the mixer).
    void open(const model::Id& track, const ui::Rect& anchor, std::function<void()> rename) const;
    // The colour swatches (model::kTrackColors and None); an empty anchor
    // centres the menu (the ui.track.color action).
    Status openColors(const model::Id& track, const ui::Rect& anchor) const;
    void openRename(const model::Id& track, const ui::Rect& anchor) const;
    // What a channel's items follow when the tempo changes (track.set timebase).
    void openTimebase(const model::Id& track, const ui::Rect& anchor) const;
    // How many channels the strip has (track.set channels).
    void openChannels(const model::Id& track, const ui::Rect& anchor) const;

  private:
    void commit(std::string_view command, json::Value args) const;
};

}  // namespace supple::gui
