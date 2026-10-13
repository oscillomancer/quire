// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The files file() properties name (core/Property.h), loaded on a worker
// thread with the processor's loadFile() and handed to its file() hook on the
// audio thread (dsp/Worker.h). The engine owns one, for a product that has
// such properties.
//
// Every request takes a ticket, and a load that finishes after a newer
// request is dropped: what the processor ends up with is always what was
// asked for last. Nothing loads before the first activation, which knows the
// sample rate; a new rate, block size or channel count reloads everything.
#pragma once

#include <concepts>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/Definition.h"
#include "core/Files.h"
#include "dsp/AudioFile.h"
#include "dsp/Worker.h"

namespace supple {

// A line about what loadFile() made, for the editor: audio describes itself,
// and a processor with its own FileContents may say how with
//   static std::string describeFile(const FileContents&);
template <class P>
std::string describeFile(const typename P::FileContents& c) {
    if constexpr (requires { { P::describeFile(c) } -> std::convertible_to<std::string>; }) {
        return P::describeFile(c);
    } else if constexpr (std::is_same_v<typename P::FileContents, dsp::AudioData>) {
        char line[64];
        std::snprintf(line, sizeof(line), "%d ch, %.2f s", c.channels(), c.seconds());
        return line;
    } else {
        return {};
    }
}

template <class P>
class FileSlots {
  public:
    using Contents = typename P::FileContents;

    // What loads depend on besides the path.
    struct Session {
        double sampleRate = 0.0;
        int maxBlockSize = 0, channels = 0;
        bool operator==(const Session&) const = default;
    };

    // For the file() properties of `list`, which outlives this.
    explicit FileSlots(const PropertyList& list) : list_(&list), slots_(list.count()) {
        for (size_t i = 0; i < list.count(); ++i)
            if (list.at(i).isFile() && list.at(i).def[0]) request(i, list.at(i).def, false);
    }
    ~FileSlots() { worker_.stop(); }
    FileSlots(const FileSlots&) = delete;
    FileSlots& operator=(const FileSlots&) = delete;

    // --- main thread -----------------------------------------------------------

    // Loads `path` (relocated already, or "") for property `index`. `wait`
    // blocks until it is in place for the audio thread to take.
    void request(size_t index, const std::string& path, bool wait) {
        Slot& slot = slots_[index];
        uint64_t ticket = 0;
        Session session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ticket = ++slot.ticket;
            slot.status = FileStatus{};
            slot.status.path = path;
            slot.status.state = path.empty()           ? FileStatus::State::Empty
                                : !files::exists(path) ? FileStatus::State::Missing
                                                       : FileStatus::State::Loading;
            session = session_;
            if (!prepared_) return;  // the first activation loads it
        }
        load(index, ticket, path, session);
        if (wait) worker_.wait();
    }

    // At activation: a session unlike the last reloads every file, and
    // waits for them.
    void prepare(const Session& session) {
        bool reload;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reload = !prepared_ || !(session == session_);
            prepared_ = true;
            session_ = session;
        }
        for (size_t i = 0; reload && i < list_->count(); ++i) {
            if (!list_->at(i).isFile()) continue;
            std::string path;
            uint64_t ticket = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Slot& slot = slots_[i];
                path = slot.status.path;
                ticket = ++slot.ticket;
                if (!path.empty() && slot.status.state != FileStatus::State::Missing)
                    slot.status.state = FileStatus::State::Loading;
            }
            load(i, ticket, path, session);
        }
        worker_.wait();
    }

    FileStatus status(size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return slots_[index].status;
    }

    // Until every load asked for so far has finished.
    void wait() { worker_.wait(); }

    // --- audio thread (or main, while it is stopped) --------------------------

    // fn(index, contents) for each slot with something new.
    template <class Fn>
    void deliver(Fn&& fn) {
        for (size_t i = 0; i < list_->count(); ++i)
            if (list_->at(i).isFile() && slots_[i].handoff.update()) fn(i, slots_[i].handoff.get());
    }
    // Every slot, new or not.
    template <class Fn>
    void deliverAll(Fn&& fn) {
        for (size_t i = 0; i < list_->count(); ++i) {
            if (!list_->at(i).isFile()) continue;
            slots_[i].handoff.update();
            fn(i, slots_[i].handoff.get());
        }
    }

  private:
    struct Slot {
        dsp::Handoff<Contents> handoff;  // published from the worker only
        FileStatus status;               // under mutex_
        uint64_t ticket = 0;             // under mutex_
    };

    void load(size_t index, uint64_t ticket, const std::string& path, const Session& session) {
        // What the audio thread lets go of is freed on the worker, now and
        // then; the thread starts with the first load.
        if (!collector_) collector_ = worker_.repeat([this] { collect(); }, 1000.0);
        worker_.post([this, index, ticket, path, session] {
            Slot& slot = slots_[index];
            auto current = [&] {
                std::lock_guard<std::mutex> lock(mutex_);
                return slot.ticket == ticket;
            };
            if (!current()) return;  // asked for something else since
            std::unique_ptr<Contents> contents;
            std::string error, summary;
            const bool there = !path.empty() && files::exists(path);
            if (there) {
                FileRequest r;
                r.id = list_->at(index).id;
                r.kind = list_->at(index).kind;
                r.path = path;
                r.sampleRate = session.sampleRate;
                r.maxBlockSize = session.maxBlockSize;
                r.channels = session.channels;
                contents = P::loadFile(r, error);
                if (!contents && error.empty()) error = "could not be loaded";
                if (contents) summary = describeFile<P>(*contents);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (slot.ticket != ticket) return;
            slot.status.state = path.empty() ? FileStatus::State::Empty
                                : !there     ? FileStatus::State::Missing
                                : contents   ? FileStatus::State::Ready
                                             : FileStatus::State::Failed;
            slot.status.error = contents ? std::string() : error;
            slot.status.summary = summary;
            slot.handoff.publish(std::move(contents));
        });
    }

    void collect() {
        for (Slot& slot : slots_) slot.handoff.collect();
    }

    const PropertyList* list_;
    std::vector<Slot> slots_;
    mutable std::mutex mutex_;
    Session session_;
    bool prepared_ = false;
    int collector_ = 0;
    dsp::Worker worker_;  // last: stopped and joined before the slots go
};

}  // namespace supple
