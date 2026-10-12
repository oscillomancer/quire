// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Saved state: a small chunked binary format.
//
//   tag magic           the product's four-character state tag, as text
//   u32 format version  1
//   chunks              tag, u32 byte length, payload
//     PARM  u32 count, then count x (u32 parameter ID, f64 host value)
//     PROP  u32 count, then count x (u32 property ID, u32 length, bytes)
//     EDIT  f64 editor scale
//     PRST  u32 flags (1: modified since loaded), u32 length, the current
//           preset's name
//
// Tags are four ASCII bytes in reading order, so a state file starts with
// the product's tag; numbers are little-endian.
//
// Parameters are saved by ID, so any reordering of the table is harmless, and
// with their host value -- the value hosts automate -- because that round-
// trips exactly: a state saved, loaded and saved again is byte-identical,
// which hosts that compare state to detect a modified project rely on. (A
// plain value would not: host -> plain -> host goes through log/exp and
// drifts by an ulp.) The consequence is that a released parameter's range and
// taper are session ABI, exactly as they already are for automation lanes.
//
// Output parameters are not state: they are the processor's report, and a
// state that changed with every meter reading would never compare equal.
//
// Unknown chunks and unknown IDs are skipped, absent parameters take their
// defaults, and nothing is applied unless the whole blob parses. A new chunk
// is a compatible addition; bump the format version only for a change an old
// reader cannot skip.
//
// A preset is the same format saved in the preset context: no editor size,
// no preset name and no perProject() property, so that loading it changes
// the sound and nothing about the session. Loading in the preset context
// likewise leaves perProject() properties alone, whatever the blob holds.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/ParamTable.h"
#include "core/PropertyTable.h"

namespace supple::state {

inline constexpr uint32_t kFormatVersion = 1;

constexpr uint32_t fourcc(const char* s) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[0])) << 24 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 16 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 8 |
           static_cast<uint32_t>(static_cast<unsigned char>(s[3]));
}

inline constexpr uint32_t kParamChunk = fourcc("PARM");
inline constexpr uint32_t kEditorChunk = fourcc("EDIT");
inline constexpr uint32_t kPropertyChunk = fourcc("PROP");
inline constexpr uint32_t kPresetChunk = fourcc("PRST");

// What the state is for (CLAP's state context): a session, or a preset. A
// duplicated instance is a session.
enum class Context { Project, Preset };

class Writer {
  public:
    void tag(uint32_t v) {
        for (int i = 3; i >= 0; --i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void f64(double v) {
        uint64_t u = 0;
        std::memcpy(&u, &v, 8);
        for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<uint8_t>(u >> (8 * i)));
    }
    void text(const std::string& v) {
        u32(static_cast<uint32_t>(v.size()));
        bytes_.insert(bytes_.end(), v.begin(), v.end());
    }
    size_t beginChunk(uint32_t chunkTag) {
        tag(chunkTag);
        const size_t at = bytes_.size();
        u32(0);
        return at;
    }
    void endChunk(size_t at) {
        const uint32_t size = static_cast<uint32_t>(bytes_.size() - at - 4);
        for (int i = 0; i < 4; ++i) bytes_[at + static_cast<size_t>(i)] = static_cast<uint8_t>(size >> (8 * i));
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

  private:
    std::vector<uint8_t> bytes_;
};

class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool tag(uint32_t& v) {
        if (size_ - pos_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v = v << 8 | data_[pos_ + static_cast<size_t>(i)];
        pos_ += 4;
        return true;
    }
    bool u32(uint32_t& v) {
        if (size_ - pos_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
        pos_ += 4;
        return true;
    }
    bool f64(double& v) {
        if (size_ - pos_ < 8) return false;
        uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
        std::memcpy(&v, &u, 8);
        pos_ += 8;
        return true;
    }
    bool text(std::string& v, size_t limit) {
        uint32_t n = 0;
        if (!u32(n) || n > remaining() || n > limit) return false;
        v.assign(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    bool skip(size_t n) {
        if (size_ - pos_ < n) return false;
        pos_ += n;
        return true;
    }
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

  private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

inline constexpr size_t kMaxPresetName = 255;
// The most state any reader takes -- a host's stream, a preset file, the
// clipboard -- so that nothing unbounded is read into memory.
inline constexpr size_t kMaxBytes = 64u << 20;

// Everything a state blob can carry, decoded and validated. Defaults until
// something sets them; `property[i]` holds a value only where `hasProperty[i]`.
struct Snapshot {
    double host[params::kCount];
    std::string property[properties::kSlots];
    bool hasProperty[properties::kSlots] = {};
    double editorScale = 0.0;  // 0 when absent
    bool hasPreset = false;    // the session remembers a preset by name
    std::string presetName;
    bool presetModified = false;

    Snapshot() {
        for (size_t i = 0; i < params::kCount; ++i) host[i] = hostDefault(params::at(i));
    }
    // Every property its default, as a fresh instance has them.
    void defaultProperties() {
        for (size_t i = 0; i < properties::kCount; ++i) {
            property[i] = properties::at(i).def;
            hasProperty[i] = true;
        }
    }
};

inline std::vector<uint8_t> save(uint32_t magic, const Snapshot& s, Context context = Context::Project) {
    const bool preset = context == Context::Preset;
    Writer w;
    w.tag(magic);
    w.u32(kFormatVersion);
    const size_t chunk = w.beginChunk(kParamChunk);
    w.u32(static_cast<uint32_t>(params::kCount - params::kOutputCount));
    for (size_t i = 0; i < params::kCount; ++i) {
        if (params::at(i).isOutput()) continue;
        w.u32(params::at(i).id);
        w.f64(s.host[i]);
    }
    w.endChunk(chunk);
    if constexpr (properties::kCount > 0) {
        uint32_t count = 0;
        for (size_t i = 0; i < properties::kCount; ++i)
            count += s.hasProperty[i] && !(preset && properties::at(i).perProjectOnly()) ? 1 : 0;
        if (count > 0) {
            const size_t props = w.beginChunk(kPropertyChunk);
            w.u32(count);
            for (size_t i = 0; i < properties::kCount; ++i) {
                if (!s.hasProperty[i] || (preset && properties::at(i).perProjectOnly())) continue;
                w.u32(properties::at(i).id);
                w.text(s.property[i]);
            }
            w.endChunk(props);
        }
    }
    if (!preset && s.editorScale > 0.0) {
        const size_t editor = w.beginChunk(kEditorChunk);
        w.f64(s.editorScale);
        w.endChunk(editor);
    }
    if (!preset && s.hasPreset && s.presetName.size() <= kMaxPresetName) {
        const size_t named = w.beginChunk(kPresetChunk);
        w.u32(s.presetModified ? 1u : 0u);
        w.text(s.presetName);
        w.endChunk(named);
    }
    return w.bytes();
}

// The longest blob save() can write, every property at its limit: no more
// than kMaxBytes, or a session could save what no reader would load back.
template <class Properties = PropertyTable>
constexpr size_t maxSavedBytes() {
    size_t n = 4 + 4;                                                // magic, version
    n += 8 + 4 + (4 + 8) * (params::kCount - params::kOutputCount);  // PARM
    if (Properties::kCount > 0) n += 8 + 4;                           // PROP
    for (size_t i = 0; i < Properties::kCount; ++i) n += 4 + 4 + Properties::at(i).limit;
    n += 8 + 8;                       // EDIT
    n += 8 + 4 + 4 + kMaxPresetName;  // PRST
    return n;
}
static_assert(maxSavedBytes() <= kMaxBytes,
              "the properties' limits add up to more state than will load back (64 MiB): lower some maxBytes()");

enum class LoadResult { Ok, NotOurs, Corrupt };

// NotOurs means the magic did not match: the caller may try a legacy loader.
inline LoadResult load(const uint8_t* data, size_t size, uint32_t magic, Snapshot& out) {
    Reader r(data, size);
    uint32_t m = 0, version = 0;
    if (!r.tag(m)) return LoadResult::Corrupt;
    if (m != magic) return LoadResult::NotOurs;
    if (!r.u32(version) || version == 0 || version > kFormatVersion) return LoadResult::Corrupt;

    Snapshot snapshot;
    bool sawParams = false;
    while (r.remaining() > 0) {
        uint32_t tag = 0, length = 0;
        if (!r.tag(tag) || !r.u32(length) || r.remaining() < length) return LoadResult::Corrupt;
        const size_t end = r.position() + length;
        if (tag == kParamChunk) {
            uint32_t count = 0;
            if (!r.u32(count) || count > length / 12) return LoadResult::Corrupt;
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t id = 0;
                double v = 0.0;
                if (!r.u32(id) || !r.f64(v)) return LoadResult::Corrupt;
                const int i = params::indexOf(id);
                if (i >= 0 && std::isfinite(v) && !params::at(static_cast<size_t>(i)).isOutput())
                    snapshot.host[i] = sanitizeHost(params::at(static_cast<size_t>(i)), v);
            }
            sawParams = true;
        } else if (tag == kPropertyChunk) {
            uint32_t count = 0;
            if (!r.u32(count) || count > length / 8) return LoadResult::Corrupt;
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t id = 0;
                std::string value;
                if (!r.u32(id) || !r.text(value, end - r.position())) return LoadResult::Corrupt;
                const int i = properties::indexOf(id);
                // A value too long for the property now is dropped, as an
                // out-of-range one would be; the default stands.
                if (i >= 0 && properties::accepts(static_cast<size_t>(i), value)) {
                    snapshot.property[i] = std::move(value);
                    snapshot.hasProperty[i] = true;
                }
            }
        } else if (tag == kPresetChunk) {
            uint32_t flags = 0;
            std::string name;
            if (!r.u32(flags) || !r.text(name, end - r.position())) return LoadResult::Corrupt;
            if (name.size() <= kMaxPresetName) {
                snapshot.hasPreset = true;
                snapshot.presetName = std::move(name);
                snapshot.presetModified = (flags & 1u) != 0;
            }
        } else if (tag == kEditorChunk && length >= 8) {
            double scale = 0.0;
            if (!r.f64(scale)) return LoadResult::Corrupt;
            if (std::isfinite(scale) && scale > 0.0) snapshot.editorScale = scale;
        }
        if (r.position() > end || !r.skip(end - r.position())) return LoadResult::Corrupt;
    }
    if (!sawParams) return LoadResult::Corrupt;
    out = snapshot;
    return LoadResult::Ok;
}

}  // namespace supple::state
