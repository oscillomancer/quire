// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Retained bridge publications, independent of the descriptor that announces them.
#pragma once

#include <algorithm>

#include "bridge_protocol.h"

namespace supple::host::bridge {

// Shutdown changes the futex word as well as waking it: a stop racing the
// wait syscall must make its expected-value check fail, not lose the wake.
inline void requestAudioQuit(std::atomic<bool>& quit, std::atomic<std::uint32_t>& request,
                             SharedSignals& signals) {
    quit.store(true, std::memory_order_release);
    request.fetch_add(1, std::memory_order_release);
    signals.wake(request);
}

struct NotificationLatch {
    std::uint64_t published = 0, announced = 0;
    bool latched = false;
    void publish() { ++published; }
    bool announce() {
        if (latched || published == announced) return false;
        announced = published;
        latched = true;
        return true;
    }
    void acknowledge() {
        announced = published;
        latched = false;
    }
};

inline bool mergeNotifications(Instance::Notifications& to, const Instance::Notifications& from) {
    to.stateDirty |= from.stateDirty;
    to.paramsRescan |= from.paramsRescan;
    to.latencyChanged |= from.latencyChanged;
    to.semanticChanged |= from.semanticChanged;
    to.tailChanged |= from.tailChanged;
    to.paramsRescanFlags |= from.paramsRescanFlags;
    to.droppedEvents += from.droppedEvents;
    if (from.restartError) to.restartError = from.restartError;
    if (from.paramsRescan) to.paramsClear.clear();
    if (to.paramsClear.size() + from.paramsClear.size() > kMaxParamClears) return false;
    to.paramsClear.insert(to.paramsClear.end(), from.paramsClear.begin(), from.paramsClear.end());
    return true;
}

inline bool hasNotifications(const Instance::Notifications& n) {
    return n.stateDirty || n.paramsRescan || n.latencyChanged || n.semanticChanged ||
           n.tailChanged || n.paramsRescanFlags || n.droppedEvents || n.restartError ||
           !n.paramsClear.empty();
}

inline void mergeEditorRequests(Instance::EditorRequests& to,
                                const Instance::EditorRequests& from) {
    if (from.resize) {
        to.width = from.width;
        to.height = from.height;
    }
    to.resize |= from.resize;
    to.closed |= from.closed;
    to.show |= from.show;
    to.hide |= from.hide;
}

inline bool hasEditorRequests(const Instance::EditorRequests& q) {
    return q.resize || q.closed || q.show || q.hide;
}

inline void mergeIdleOutputs(Instance::IdleOutputs& to, const Instance::IdleOutputs& from) {
    const auto count =
        std::min(from.events.size(), Instance::IdleOutputs::kCapacity - to.events.size());
    to.events.insert(to.events.end(), from.events.begin(), from.events.begin() + count);
    to.dropped += from.dropped + from.events.size() - count;
}

}  // namespace supple::host::bridge
