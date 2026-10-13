// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/semantic_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "supple/clap/editor_picture.h"

namespace supple::suite {

bool writeAll(const clap_ostream_t* out, const std::string& text) {
    if (!out || !out->write) return false;
    size_t done = 0;
    while (done < text.size()) {
        const int64_t n = out->write(out, text.data() + done, text.size() - done);
        if (n <= 0 || static_cast<uint64_t>(n) > text.size() - done) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

namespace {

SemanticLayer* layerOf(const clap_plugin_t* plugin) {
    return dynamic_cast<SemanticLayer*>(plugin::Layer::of(plugin));
}

const clap_plugin_supple_semantic_params_t kExtension = {
    sizeof(clap_plugin_supple_semantic_params_t),
    [](const clap_plugin_t* p, const clap_ostream_t* out) {
        const SemanticLayer* l = layerOf(p);
        return l && l->getSchema(out);
    },
    [](const clap_plugin_t* p, uint32_t detail, const clap_ostream_t* out) {
        const SemanticLayer* l = layerOf(p);
        return l && l->getPatch(detail, out);
    },
    [](const clap_plugin_t* p, uint32_t mode, const char* json, uint32_t size,
       const clap_ostream_t* errors) {
        SemanticLayer* l = layerOf(p);
        return l && l->setPatch(mode, json, size, errors);
    },
    [](const clap_plugin_t* p, uint32_t detail, const clap_ostream_t* out) {
        const SemanticLayer* l = layerOf(p);
        return l && l->describe(detail, out);
    },
    nullptr,  // no live telemetry
    [](const clap_plugin_t* p, const clap_ostream_t* out) {
        const SemanticLayer* l = layerOf(p);
        return l && l->getFactoryPatches(out);
    },
};

static_assert(static_cast<int>(PointerInput::Kind::Leave) == CLAP_SUPPLE_POINTER_LEAVE &&
              static_cast<int>(PointerInput::Kind::Wheel) == CLAP_SUPPLE_POINTER_WHEEL &&
              static_cast<int>(PointerInput::Kind::Release) == CLAP_SUPPLE_POINTER_RELEASE &&
              static_cast<int>(PointerInput::Kind::Press) == CLAP_SUPPLE_POINTER_PRESS);

// The editor drawn without a window (plugin/Layer.h's drawEditor), after
// the pointer input: its size, then its pixels.
const clap_plugin_supple_editor_picture_t kPicture = {
    sizeof(clap_plugin_supple_editor_picture_t),
    [](const clap_plugin_t* p, bool on) {
        if (SemanticLayer* l = layerOf(p)) l->watchEditor(on);
    },
    [](const clap_plugin_t* p, double scale, const clap_supple_pointer_input_t* input,
       uint32_t count, const clap_ostream_t* out, const clap_ostream_t* why) {
        SemanticLayer* l = layerOf(p);
        return l && l->drawEditor(scale, input, count, out, why);
    },
};

json::Error error(std::string code, std::string message, std::string hint) {
    json::Error e;
    e.code = std::move(code);
    e.message = std::move(message);
    e.hint = std::move(hint);
    return e;
}

}  // namespace

SemanticLayer::SemanticLayer(plugin::Instance& instance) : instance_(instance) { rebuild(); }

void SemanticLayer::rebuild() {
    const Definition& d = *instance_.currentDefinition();
    auto codec = std::make_unique<PatchCodec>(d);
    if (const std::string problems = meaningProblems(d, codec->meaning()); !problems.empty()) {
        instance_.log(CLAP_LOG_WARNING, "no semantic view: " + problems);
        codec_.reset();
        return;
    }
    codec->setFileCheck([this](size_t index, const std::string& path, std::string& why) {
        return fileReadable(index, path, why);
    });
    codec_ = std::move(codec);
}

const void* SemanticLayer::extension(const char* id) {
    if (std::strcmp(id, CLAP_EXT_SUPPLE_EDITOR_PICTURE) == 0) return &kPicture;
    return codec_ && std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS) == 0 ? &kExtension : nullptr;
}

void SemanticLayer::watchEditor(bool on) { instance_.watch(on); }

bool SemanticLayer::drawEditor(double scale, const clap_supple_pointer_input_t* input,
                               uint32_t count, const clap_ostream_t* out,
                               const clap_ostream_t* why) {
    std::vector<PointerInput> pointer;
    if (input)
        for (uint32_t i = 0; i < count; ++i) {
            const clap_supple_pointer_input_t& in = input[i];
            if (in.kind > CLAP_SUPPLE_POINTER_LEAVE) continue;
            auto place = [](double v) {
                return static_cast<float>(std::isfinite(v) ? std::clamp(v, -1e5, 1e5) : 0.0);
            };
            PointerInput p;
            p.kind = static_cast<PointerInput::Kind>(in.kind);
            p.x = place(in.x);
            p.y = place(in.y);
            p.wheel = static_cast<float>(
                std::isfinite(in.wheel) ? std::clamp(in.wheel, -100.0, 100.0) : 0.0);
            p.doubleClick = (in.flags & CLAP_SUPPLE_POINTER_DOUBLE_CLICK) != 0;
            p.shift = (in.flags & CLAP_SUPPLE_POINTER_SHIFT) != 0;
            p.ctrl = (in.flags & CLAP_SUPPLE_POINTER_CTRL) != 0;
            p.alt = (in.flags & CLAP_SUPPLE_POINTER_ALT) != 0;
            pointer.push_back(p);
        }
    Picture picture;
    std::string problem;
    if (!instance_.drawEditor(scale, pointer, picture, problem)) {
        if (why) writeAll(why, problem);
        return false;
    }
    std::string bytes(8, '\0');
    for (int k = 0; k < 4; ++k) {
        bytes[static_cast<size_t>(k)] =
            static_cast<char>((static_cast<uint32_t>(picture.width) >> (8 * k)) & 0xFF);
        bytes[static_cast<size_t>(4 + k)] =
            static_cast<char>((static_cast<uint32_t>(picture.height) >> (8 * k)) & 0xFF);
    }
    bytes.append(reinterpret_cast<const char*>(picture.rgba.data()), picture.rgba.size());
    return writeAll(out, bytes);
}

void SemanticLayer::changed(uint32_t flags) {
    const auto* host = static_cast<const clap_host_supple_semantic_params_t*>(
        instance_.hostExtension(CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (host && host->struct_size >= sizeof(clap_host_supple_semantic_params_t) && host->changed)
        host->changed(instance_.clapHost(), flags);
}

void SemanticLayer::redefined() {
    rebuild();
    changed(CLAP_SUPPLE_CHANGED_SCHEMA | CLAP_SUPPLE_CHANGED_PATCH);
}

bool SemanticLayer::getSchema(const clap_ostream_t* out) const {
    return codec_ && writeAll(out, json::writeCompact(codec_->schemaDocument()));
}

bool SemanticLayer::getPatch(uint32_t detail, const clap_ostream_t* out) const {
    if (!codec_) return false;
    const json::Value doc =
        codec_->patch(instance_.snapshot(), envelope_, detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED);
    return writeAll(out, json::writeCanonical(doc));
}

bool SemanticLayer::setPatch(uint32_t mode, const char* text, uint32_t size,
                             const clap_ostream_t* errors) {
    std::vector<json::Error> found;
    auto fail = [&] {
        if (errors) writeAll(errors, json::errorsToJson(found));
        return false;
    };
    if (!codec_) return false;
    if (mode > CLAP_SUPPLE_SET_VALIDATE_ONLY) {
        found.push_back(error("patch.mode",
                              "set_patch mode " + std::to_string(mode) + " is not one there is",
                              "use REPLACE (0), MERGE (1) or VALIDATE_ONLY (2)"));
        return fail();
    }
    if (!text && size) {
        found.push_back(error("patch.input", "the patch text is null", "send the text"));
        return fail();
    }
    json::Value doc;
    json::Error parseError;
    if (!json::parse(std::string_view(text ? text : "", size), doc, parseError)) {
        found.push_back(std::move(parseError));
        return fail();
    }
    state::Snapshot snapshot = instance_.snapshot();
    Envelope envelope = envelope_;
    if (!codec_->apply(doc, mode != CLAP_SUPPLE_SET_REPLACE, snapshot, envelope, found))
        return fail();
    if (mode == CLAP_SUPPLE_SET_VALIDATE_ONLY) return true;
    envelope_ = std::move(envelope);
    // Everything the patch holds, its session properties included; the
    // preset it came from and the editor's size stay as they were.
    instance_.applyValues(snapshot);
    instance_.markDirty();
    changed(CLAP_SUPPLE_CHANGED_PATCH);
    return true;
}

bool SemanticLayer::describe(uint32_t detail, const clap_ostream_t* out) const {
    return codec_ && writeAll(out, codec_->describe(instance_.snapshot(),
                                                    detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED));
}

bool SemanticLayer::getFactoryPatches(const clap_ostream_t* out) const {
    if (!codec_) return false;
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.factory-patches/1.0";
    json::Value patches = json::Value::array();
    const Definition& d = *instance_.currentDefinition();
    for (const auto& preset : d.presets()) {
        state::Snapshot s(d);
        s.defaultProperties();
        for (const auto& [id, plain] : preset.values) {
            const int i = d.params().indexOf(id);
            if (i >= 0)
                s.host[static_cast<size_t>(i)] =
                    toHost(d.params().at(static_cast<size_t>(i)), plain);
        }
        for (const auto& [id, text] : preset.texts) {
            const int i = d.properties().indexOf(id);
            if (i >= 0) s.property[static_cast<size_t>(i)] = text;
        }
        Envelope envelope;
        envelope.name = preset.name;
        patches.push_back(codec_->patch(s, envelope, false));
    }
    doc["patches"] = std::move(patches);
    return writeAll(out, json::writeCanonical(doc));
}

std::vector<uint8_t> SemanticLayer::saveChunk(const state::Snapshot& snapshot, state::Context) {
    if (!codec_) return {};
    const uint32_t magic = state::fourcc(snapshot.definition->info().stateTag.c_str());
    PatchCodec codec(
        *snapshot.definition);  // the snapshot's, which a redefinition may have left behind
    return frame(json::writeCanonical(codec.patch(snapshot, envelope_, false)), magic);
}

bool SemanticLayer::loadChunk(const uint8_t* data, size_t size, state::Snapshot& snapshot,
                              state::Context) {
    if (!codec_ || size == 0) return true;  // nothing to prefer: the framework's values stand
    std::string text;
    json::Value doc;
    json::Error parseError;
    if (!unframe(data, size, state::fourcc(snapshot.definition->info().stateTag.c_str()), text) ||
        !json::parse(text, doc, parseError) || !doc.isObject())
        return false;  // damaged
    PatchCodec codec(*snapshot.definition);
    codec.restore(doc, snapshot, envelope_);
    return true;
}

}  // namespace supple::suite
