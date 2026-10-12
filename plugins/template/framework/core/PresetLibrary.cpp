// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "core/PresetLibrary.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "core/Files.h"

namespace supple {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool writeFile(const fs::path& file, const std::vector<uint8_t>& bytes) {
    return files::writeAtomic(file, bytes.data(), bytes.size());
}

bool ours(const fs::path& file, const std::string& stateTag) {
    std::ifstream in(file, std::ios::binary);
    char tag[4] = {};
    return stateTag.size() == 4 && in.read(tag, 4) && std::equal(tag, tag + 4, stateTag.begin());
}

}  // namespace

// --- names and folders ------------------------------------------------------------------------

fs::path PresetLibrary::fromUtf8(std::string_view text) { return files::fromUtf8(text); }

std::string PresetLibrary::toUtf8(const fs::path& path) { return files::toUtf8(path); }

#if !SUPPLE_PRODUCT_RUNTIME
fs::path PresetLibrary::defaultUserFolder() { return defaultUserFolder(productDefinition()->info()); }
#endif

fs::path PresetLibrary::defaultUserFolder(const PluginInfo& info) {
    if (const char* forced = std::getenv("SUPPLE_PRESETS_DIR"); forced && *forced) return fromUtf8(forced);
    const fs::path product = fromUtf8(info.vendor) / fromUtf8(info.name);
#if defined(_WIN32)
    if (const wchar_t* appData = _wgetenv(L"APPDATA"); appData && *appData)
        return fs::path(appData) / product / "Presets";
    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return fromUtf8(home) / "Library" / "Audio" / "Presets" / product;
    return {};
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data == '/')
        return fromUtf8(data) / product / "presets";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fromUtf8(home) / ".local" / "share" / product / "presets";
    return {};
#endif
}

bool PresetLibrary::validName(std::string_view name) {
    if (name.empty() || name.size() > kMaxName) return false;
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.') return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return false;
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) return false;
    }
    // Windows refuses these as file names, with or without an extension.
    std::string stem = lower(name.substr(0, name.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (const char* reserved : {"con", "prn", "aux", "nul"})
        if (stem == reserved) return false;
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9')
        return false;
    return true;
}

// --- the library ---------------------------------------------------------------------------------

PresetLibrary::PresetLibrary(StateAccess& state, fs::path userFolder, DefinitionPtr definition)
    : state_(state),
      definition_(std::move(definition)),
      magic_(state::fourcc(definition_->info().stateTag.c_str())),
      folder_(std::move(userFolder)) {
    refresh();
}

void PresetLibrary::refresh() {
    const std::string currentName = current() >= 0 ? entries_[static_cast<size_t>(current_)].name : "";
    const bool currentUser = current() >= 0 && entries_[static_cast<size_t>(current_)].user();
    entries_.clear();
    const auto& factory = definition_->presets();
    for (size_t i = 0; i < factory.size(); ++i) entries_.push_back({factory[i].name, factory[i].category, static_cast<int>(i), {}});
    if (!folder_.empty()) {
        std::vector<Entry> user;
        std::error_code ec;
        for (fs::directory_iterator it(folder_, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& file = it->path();
            if (!it->is_regular_file(ec) || file.extension() != kExtension) continue;
            const std::string name = toUtf8(file.stem());
            if (!validName(name) || !ours(file, definition_->info().stateTag)) continue;
            user.push_back({name, "", -1, file});
        }
        std::sort(user.begin(), user.end(), [](const Entry& a, const Entry& b) {
            const std::string la = lower(a.name), lb = lower(b.name);
            return la != lb ? la < lb : a.name < b.name;
        });
        entries_.insert(entries_.end(), user.begin(), user.end());
    }
    current_ = currentName.empty() ? -1 : findEntry(currentName, currentUser);
    ++revision_;
}

int PresetLibrary::findEntry(std::string_view name, bool user) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].user() == user && entries_[i].name == name) return static_cast<int>(i);
    return -1;
}

fs::path PresetLibrary::fileFor(std::string_view name) const {
    fs::path file = folder_ / fromUtf8(name);
    file += kExtension;
    return file;
}

bool PresetLibrary::modified() {
    if (stickyModified_) return true;
    const std::vector<uint8_t> now = state_.saveState(state::Context::Preset);
    if (!baseline_.empty()) return now != baseline_;
    return name_ == "Default" ? now != defaultsBlob() : false;
}

bool PresetLibrary::apply(const std::vector<uint8_t>& blob) {
    return !blob.empty() && state_.loadState(blob.data(), blob.size(), state::Context::Preset);
}

void PresetLibrary::loaded(int index, std::string name) {
    current_ = index >= 0 && static_cast<size_t>(index) < entries_.size() ? index : -1;
    name_ = std::move(name);
    baseline_ = state_.saveState(state::Context::Preset);
    stickyModified_ = false;
}

std::vector<uint8_t> PresetLibrary::defaultsBlob() const {
    state::Snapshot s(*definition_);
    s.defaultProperties();
    return state::save(magic_, s, state::Context::Preset);
}

std::vector<uint8_t> PresetLibrary::factoryBlob(size_t index) const {
    state::Snapshot s(*definition_);
    s.defaultProperties();
    const ParamTable& params = definition_->params();
    const PropertyList& properties = definition_->properties();
    const FactoryPreset& preset = definition_->presets()[index];
    for (const auto& [id, plain] : preset.values) {
        const int i = params.indexOf(id);
        if (i < 0) continue;
        const ParamDef& p = params.at(static_cast<size_t>(i));
        s.host[static_cast<size_t>(i)] = sanitizeHost(p, toHost(p, plain));
    }
    for (const auto& [id, text] : preset.texts) {
        const int i = properties.indexOf(id);
        if (i >= 0) s.property[static_cast<size_t>(i)] = text;
    }
    return state::save(magic_, s, state::Context::Preset);
}

bool PresetLibrary::load(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
    const Entry entry = entries_[static_cast<size_t>(index)];
    const bool ok = entry.user() ? loadFile(entry.file) : loadFactory(static_cast<size_t>(entry.factory));
    if (ok && onLoaded) onLoaded(entry);
    return ok;
}

bool PresetLibrary::step(int delta) {
    const int n = static_cast<int>(entries_.size());
    if (n == 0) return false;
    const int from = current() >= 0 ? current() : delta > 0 ? -1 : 0;
    return load(((from + delta) % n + n) % n);
}

bool PresetLibrary::loadFactory(size_t index) {
    if (index >= definition_->presets().size() || !apply(factoryBlob(index))) return false;
    const std::string& name = definition_->presets()[index].name;
    loaded(findEntry(name, false), name);
    return true;
}

bool PresetLibrary::loadFile(const fs::path& file) {
    std::vector<uint8_t> bytes;
    if (!files::readBounded(file, bytes, state::kMaxBytes) || !apply(bytes)) return false;
    // A file in the user folder is listed under its name; one from
    // elsewhere (a host's browser) is current by name only.
    std::error_code ec;
    int index = -1;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].user() && fs::equivalent(entries_[i].file, file, ec)) index = static_cast<int>(i);
    loaded(index, toUtf8(file.stem()));
    return true;
}

bool PresetLibrary::exportFile(const fs::path& file) {
    return !file.empty() && writeFile(file, state_.saveState(state::Context::Preset));
}

bool PresetLibrary::loadDefaults() {
    if (!apply(defaultsBlob())) return false;
    loaded(-1, "Default");
    return true;
}

PresetLibrary::SaveResult PresetLibrary::save(std::string_view name, bool overwrite) {
    if (!validName(name)) return SaveResult::InvalidName;
    if (folder_.empty()) return SaveResult::Failed;
    const fs::path file = fileFor(name);
    std::error_code ec;
    if (!overwrite && fs::exists(file, ec)) return SaveResult::Exists;
    fs::create_directories(folder_, ec);
    const std::vector<uint8_t> bytes = state_.saveState(state::Context::Preset);
    if (!writeFile(file, bytes)) return SaveResult::Failed;
    refresh();
    loaded(findEntry(name, true), std::string(name));
    return SaveResult::Ok;
}

bool PresetLibrary::rename(int index, std::string_view name) {
    if (index < 0 || index >= static_cast<int>(entries_.size()) || !validName(name)) return false;
    const Entry entry = entries_[static_cast<size_t>(index)];
    if (!entry.user()) return false;
    const fs::path target = fileFor(name);
    std::error_code ec;
    // A change of case only is the same file on a case-insensitive disk.
    if (fs::exists(target, ec) && !fs::equivalent(entry.file, target, ec)) return false;
    fs::rename(entry.file, target, ec);
    if (ec) return false;
    for (Slot& slot : slots_) {
        if (slot.file == entry.file) {
            slot.file = target;
            slot.name = std::string(name);
        }
    }
    const bool wasCurrent = index == current();
    refresh();
    if (wasCurrent) {
        current_ = findEntry(name, true);
        name_ = std::string(name);
    }
    return true;
}

bool PresetLibrary::remove(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
    const Entry entry = entries_[static_cast<size_t>(index)];
    if (!entry.user()) return false;
    std::error_code ec;
    if (!fs::remove(entry.file, ec)) return false;
    // The sound stays; it is just no longer a preset.
    if (index == current()) {
        current_ = -1;
        name_.clear();
        baseline_.clear();
    }
    refresh();
    return true;
}

// --- A/B -------------------------------------------------------------------------------------

PresetLibrary::Slot PresetLibrary::capture() {
    Slot s;
    s.used = true;
    s.state = state_.saveState(state::Context::Preset);
    s.name = name_;
    if (current() >= 0) {
        const Entry& entry = entries_[static_cast<size_t>(current())];
        if (entry.user()) s.file = entry.file;
        else s.factoryName = entry.name;
    }
    s.baseline = baseline_;
    s.stickyModified = stickyModified_;
    return s;
}

void PresetLibrary::compare(int slot) {
    if (slot < 0 || slot > 1 || slot == slot_) return;
    slots_[slot_] = capture();
    if (!slots_[slot].used) slots_[slot] = slots_[slot_];
    const Slot& to = slots_[slot];
    if (!apply(to.state)) return;
    slot_ = slot;
    name_ = to.name;
    current_ = -1;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        if (entry.user() ? !to.file.empty() && entry.file == to.file
                         : !to.factoryName.empty() && entry.name == to.factoryName)
            current_ = static_cast<int>(i);
    }
    baseline_ = to.baseline;
    stickyModified_ = to.stickyModified;
    if (current() < 0 && (!to.file.empty() || !to.factoryName.empty())) {
        name_.clear();
        baseline_.clear();
        stickyModified_ = true;
    }
}

void PresetLibrary::copyToOther() { slots_[1 - slot_] = capture(); }

// --- text ------------------------------------------------------------------------------------

std::string PresetLibrary::copyText() {
    return definition_->info().name + " state: " + base64::encode(state_.saveState(state::Context::Preset));
}

bool PresetLibrary::pasteText(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    const size_t start = text.find_last_of(" \t\r\n:");
    const std::string_view payload = start == std::string_view::npos ? text : text.substr(start + 1);
    std::vector<uint8_t> bytes;
    if (!base64::decode(payload, bytes) || !apply(bytes)) return false;
    current_ = -1;
    name_.clear();
    baseline_ = state_.saveState(state::Context::Preset);
    stickyModified_ = false;
    return true;
}

// --- the session -------------------------------------------------------------------------------

void PresetLibrary::restore(const state::Snapshot& session) {
    slots_[0] = Slot{};
    slots_[1] = Slot{};
    slot_ = 0;
    if (!session.hasPreset) {
        current_ = -1;
        name_.clear();
        baseline_.clear();
        stickyModified_ = false;
        return;
    }
    // A user preset is preferred over a factory one of the same name: the
    // user saved it more recently than we shipped it.
    refresh();
    current_ = findEntry(session.presetName, true);
    if (current_ < 0) current_ = findEntry(session.presetName, false);
    name_ = session.presetName;
    stickyModified_ = session.presetModified;
    baseline_ = session.presetModified ? std::vector<uint8_t>{} : state_.saveState(state::Context::Preset);
}

void PresetLibrary::describe(state::Snapshot& session) {
    session.hasPreset = !name_.empty();
    session.presetName = name_;
    session.presetModified = session.hasPreset && modified();
}

// --- base64 ------------------------------------------------------------------------------------

namespace base64 {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string encode(const std::vector<uint8_t>& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16 |
                           (i + 1 < bytes.size() ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0u) |
                           (i + 2 < bytes.size() ? static_cast<uint32_t>(bytes[i + 2]) : 0u);
        out += kAlphabet[n >> 18 & 63];
        out += kAlphabet[n >> 12 & 63];
        out += i + 1 < bytes.size() ? kAlphabet[n >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

bool decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty() || text.size() % 4 != 0 || text.size() > (state::kMaxBytes + 2) / 3 * 4) return false;
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=') : 0;
        if (pad == 1 && text[i + 2] == '=') return false;
        int v[4];
        for (int k = 0; k < 4; ++k) {
            v[k] = k >= 4 - pad ? 0 : value(text[i + static_cast<size_t>(k)]);
            if (v[k] < 0) return false;
        }
        const uint32_t n = static_cast<uint32_t>(v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3]);
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8 & 0xFF));
        if (pad < 1) out.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    return true;
}

}  // namespace base64

}  // namespace supple
