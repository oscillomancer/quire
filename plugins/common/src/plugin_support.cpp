#include "supple/clap/plugin_support.h"

#include <cstdio>
#include <cstring>

#include "supple/json/json.h"

namespace supple::clapx {

namespace {

bool write(const clap_ostream_t* os, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const int64_t n = os->write(os, s.data() + off, s.size() - off);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool readAll(const clap_istream_t* is, std::vector<std::uint8_t>& out) {
    std::uint8_t tmp[4096];
    for (;;) {
        const int64_t n = is->read(is, tmp, sizeof(tmp));
        if (n < 0) return false;
        if (n == 0) return true;
        if (out.size() + static_cast<std::size_t>(n) > patch::kMaxStateBytes + 12) return false;
        out.insert(out.end(), tmp, tmp + n);
    }
}

json::Value errorJson(const Error& e) {
    json::Value v = json::Value::object();
    v["code"] = e.code;
    v["message"] = e.message;
    v["path"] = e.path;
    v["hint"] = e.hint;
    if (!e.details.empty()) {
        json::Value d = json::Value::array();
        for (const auto& x : e.details) d.push_back(errorJson(x));
        v["details"] = std::move(d);
    }
    return v;
}

void copyName(char* dst, std::size_t size, std::string_view src) {
    const std::size_t n = std::min(size - 1, src.size());
    if (n) std::memcpy(dst, src.data(), n);  // an empty string_view may have a null data()
    dst[n] = 0;
}

}  // namespace

std::string errorsToJson(const Error& error) {
    json::Value arr = json::Value::array();
    if (error.code == "schema.invalid" && !error.details.empty()) {
        for (const auto& d : error.details) arr.push_back(errorJson(d));
    } else {
        arr.push_back(errorJson(error));
    }
    return json::writeCompact(arr);
}

// --- ParamStore --------------------------------------------------------------------

ParamStore::ParamStore(params::Table table)
    : table_(table), values_(std::make_unique<std::atomic<double>[]>(table.size())) {
    resetToDefaults();
}

double ParamStore::get(std::size_t index) const SUPPLE_RT {
    return values_[index].load(std::memory_order_relaxed);
}

void ParamStore::set(std::size_t index, double physical) SUPPLE_RT {
    values_[index].store(params::clampPhysical(table_[index], physical), std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

bool ParamStore::getClap(clap_id id, double& clapValue) const SUPPLE_RT {
    for (std::size_t i = 0; i < table_.size(); ++i)
        if (table_[i].id == id) {
            clapValue = params::toClap(table_[i], get(i));
            return true;
        }
    return false;
}

bool ParamStore::setClap(clap_id id, double clapValue) SUPPLE_RT {
    for (std::size_t i = 0; i < table_.size(); ++i)
        if (table_[i].id == id) {
            if (table_[i].flags & params::kReadonly) return false;
            set(i, params::fromClap(table_[i], clapValue));
            return true;
        }
    return false;
}

void ParamStore::resetToDefaults() {
    for (std::size_t i = 0; i < table_.size(); ++i)
        values_[i].store(table_[i].def, std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

patch::FlatBody ParamStore::snapshot() const {
    patch::FlatBody b;
    b.values.resize(table_.size());
    for (std::size_t i = 0; i < table_.size(); ++i) b.values[i] = get(i);
    return b;
}

void ParamStore::assign(const patch::FlatBody& body) {
    for (std::size_t i = 0; i < table_.size() && i < body.values.size(); ++i) {
        if (table_[i].flags & params::kReadonly) continue;
        values_[i].store(params::clampPhysical(table_[i], body.values[i]),
                         std::memory_order_relaxed);
    }
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

// --- ClapParams --------------------------------------------------------------------

bool ClapParams::info(params::Table t, std::uint32_t index, clap_param_info_t* out) {
    if (index >= t.size()) return false;
    const auto& p = t[index];
    std::memset(out, 0, sizeof(*out));
    out->id = p.id;
    out->flags = params::clapFlags(p);
    out->cookie = nullptr;
    copyName(out->name, sizeof(out->name), p.name);
    copyName(out->module, sizeof(out->module), p.group);
    const auto r = params::clapRange(p);
    out->min_value = r.min;
    out->max_value = r.max;
    out->default_value = r.def;
    return true;
}

bool ClapParams::valueToText(params::Table t, clap_id id, double clapValue, char* out,
                             std::uint32_t size) {
    const auto* p = params::findById(t, id);
    if (!p || size == 0) return false;
    copyName(out, size, params::format(*p, params::fromClap(*p, clapValue)));
    return true;
}

bool ClapParams::textToValue(params::Table t, clap_id id, const char* text, double* clapValue) {
    const auto* p = params::findById(t, id);
    if (!p || !text) return false;
    auto v = params::parse(*p, text);
    if (!v) return false;
    *clapValue = params::toClap(*p, *v);
    return true;
}

bool ClapParams::applyEvents(ParamStore& store, const clap_input_events_t* in) SUPPLE_RT {
    bool changed = false;
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        changed = store.setClap(e->param_id, e->value) || changed;
    }
    return changed;
}

// --- FlatSemantic -----------------------------------------------------------------

FlatSemantic::FlatSemantic(const patch::FlatPatchType& type, ParamStore& store,
                           std::string pluginId, std::string docs, std::string describePrefix,
                           std::uint32_t stateMagic)
    : type_(type),
      store_(store),
      pluginId_(std::move(pluginId)),
      docs_(std::move(docs)),
      describePrefix_(std::move(describePrefix)),
      stateMagic_(stateMagic) {
    if (type_.codec().hasSeed()) envelope_.seed = 0;
}

patch::Patch<patch::FlatBody> FlatSemantic::current() const {
    patch::Patch<patch::FlatBody> p;
    p.meta = envelope_;
    p.body = store_.snapshot();
    return p;
}

bool FlatSemantic::getSchema(const clap_ostream_t* out) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.semantic-schema/1.0";
    doc["plugin"]["id"] = pluginId_;
    doc["patch_type"] = type_.schemaId();
    doc["params"] = *params::describeTable(type_.table()).find("params");
    doc["patch"] = type_.jsonSchema();
    doc["docs"] = docs_;
    return write(out, json::writeCompact(doc));
}

bool FlatSemantic::getPatch(std::uint32_t detail, const clap_ostream_t* out) const {
    return write(out, type_.serialize(current(), detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED
                                                     ? patch::Detail::Changed
                                                     : patch::Detail::Full));
}

bool FlatSemantic::setPatch(std::uint32_t mode, const char* text, std::uint32_t size,
                            const clap_ostream_t* errors) {
    auto fail = [&](const Error& e) {
        if (errors) write(errors, errorsToJson(e));
        return false;
    };
    if (mode > CLAP_SUPPLE_SET_VALIDATE_ONLY)
        return fail(Error("patch.mode", "unknown set_patch mode " + std::to_string(mode)));
    auto doc = json::parse(std::string_view(text ? text : "", size));
    if (!doc) return fail(doc.error());
    const auto before = current();
    auto result =
        mode == CLAP_SUPPLE_SET_REPLACE ? type_.fromJson(*doc) : type_.merge(before, *doc);
    if (!result) return fail(result.error());
    if (mode == CLAP_SUPPLE_SET_VALIDATE_ONLY) return true;
    envelope_ = result->meta;
    store_.assign(result->body);
    if (onApplied) onApplied(before.body, result->body);
    return true;
}

bool FlatSemantic::describe(std::uint32_t detail, const clap_ostream_t* out) const {
    const auto d =
        detail == CLAP_SUPPLE_PATCH_DETAIL_CHANGED ? patch::Detail::Changed : patch::Detail::Full;
    std::string text = describePrefix_ + patch::describeFlat(type_, current(), d);
    if (d == patch::Detail::Changed && text.size() > 600) text = text.substr(0, 597) + "...";
    return write(out, text);
}

bool FlatSemantic::saveState(const clap_ostream_t* out) const {
    const auto bytes = patch::encodeState(type_.serialize(current()), stateMagic_);
    std::size_t off = 0;
    while (off < bytes.size()) {
        const int64_t n = out->write(out, bytes.data() + off, bytes.size() - off);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool FlatSemantic::loadState(const clap_istream_t* in) {
    std::vector<std::uint8_t> bytes;
    if (!readAll(in, bytes)) return false;
    auto text = patch::decodeState(bytes, stateMagic_);
    if (!text) {
        if (legacyLoad && text.error().code != "patch.state.utf8") {
            ParamStore scratch(store_.table());
            if (!legacyLoad(bytes, scratch)) return false;
            const auto before = current();
            store_.assign(scratch.snapshot());
            if (onApplied) onApplied(before.body, store_.snapshot());
            return true;
        }
        return false;
    }
    auto p = type_.parse(*text);
    if (!p) return false;
    const auto before = current();
    envelope_ = p->meta;
    store_.assign(p->body);
    if (onApplied) onApplied(before.body, p->body);
    return true;
}

}  // namespace supple::clapx
