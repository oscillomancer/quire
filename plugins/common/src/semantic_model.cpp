#include "supple/clap/semantic_model.h"

#include <vector>

#include "supple/clap/plugin_support.h"
#include "supple/patch/patch.h"

namespace supple::clapx {

namespace {

bool writeAll(const clap_ostream_t* out, std::string_view text) {
    if (!out || !out->write) return false;
    std::size_t off = 0;
    while (off < text.size()) {
        const int64_t n = out->write(out, text.data() + off, text.size() - off);
        if (n <= 0 || static_cast<std::uint64_t>(n) > text.size() - off) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool readStream(const clap_istream_t* in, std::vector<std::uint8_t>& bytes) {
    if (!in || !in->read) return false;
    std::uint8_t buf[4096];
    for (;;) {
        const int64_t n = in->read(in, buf, sizeof buf);
        if (n < 0 || n > static_cast<int64_t>(sizeof buf)) return false;
        if (n == 0) return true;
        bytes.insert(bytes.end(), buf, buf + n);
        if (bytes.size() > patch::kMaxStateBytes + 16) return false;
    }
}

}  // namespace

SemanticAdapter::SemanticAdapter(SemanticModel& model, Identity identity)
    : model_(model), id_(std::move(identity)) {}

bool SemanticAdapter::getSchema(const clap_ostream_t* out) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.semantic-schema/1.0";
    doc["plugin"]["id"] = id_.id;
    doc["plugin"]["name"] = id_.name;
    doc["plugin"]["version"] = id_.version;
    doc["patch_type"] = model_.patchTypeId();
    json::Value ps = *params::describeTable(model_.table()).find("params");
    const auto t = model_.table();
    for (std::size_t i = 0; i < t.size(); ++i)
        if (!model_.paramActive(i)) ps[t[i].key]["active"] = false;
    doc["params"] = std::move(ps);
    doc["patch"] = model_.patchSchema();
    doc["docs"] = id_.docs;
    return writeAll(out, json::writeCompact(doc));
}

bool SemanticAdapter::getPatch(std::uint32_t detail, const clap_ostream_t* out) const {
    return writeAll(out, model_.patchJson(detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED));
}

bool SemanticAdapter::setPatch(std::uint32_t mode, const char* text, std::uint32_t size,
                               const clap_ostream_t* errors) {
    auto fail = [&](const Error& e) {
        if (errors) writeAll(errors, errorsToJson(e));
        return false;
    };
    if (mode > CLAP_SUPPLE_SET_VALIDATE_ONLY)
        return fail(Error("patch.mode", "unknown set_patch mode " + std::to_string(mode)));
    if (!text && size) return fail(Error("patch.input", "patch text is null"));
    auto doc = json::parse(std::string_view(text ? text : "", size));
    if (!doc) return fail(doc.error());
    auto st = model_.apply(mode, *doc);
    if (!st) return fail(st.error());
    return true;
}

bool SemanticAdapter::describe(std::uint32_t detail, const clap_ostream_t* out) const {
    return writeAll(out, model_.describe(detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED));
}

bool SemanticAdapter::saveState(const clap_ostream_t* out) const {
    const auto framed = patch::encodeState(model_.patchJson(false), id_.stateMagic);
    if (!framed) return false;
    return writeAll(
        out, std::string_view(reinterpret_cast<const char*>(framed->data()), framed->size()));
}

bool SemanticAdapter::loadState(const clap_istream_t* in) {
    std::vector<std::uint8_t> bytes;
    if (!readStream(in, bytes)) return false;
    auto text = patch::decodeState(bytes, id_.stateMagic);
    if (!text) return false;
    auto doc = json::parse(*text);
    if (!doc) return false;
    return static_cast<bool>(model_.apply(CLAP_SUPPLE_SET_REPLACE, *doc));
}

}  // namespace supple::clapx
