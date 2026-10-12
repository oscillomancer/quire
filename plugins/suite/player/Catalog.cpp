// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player's catalog (core/Catalog.h): a plugin for each script in the
// library -- the folders SUPPLE_SCRIPT_LIBRARY names -- at
// <folder>/<name>/<name>.cpp, each with the ID, name and kind its script
// declares. A script that does not compile, or whose plugin would break the
// framework's rules, is left out; its diagnostics are what dsp.check
// reports. Two scripts claiming one ID: the first wins, by folder in the
// order given, then by name.
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

#include "Catalog.h"
#include "core/Catalog.h"
#include "core/Files.h"
#include "supple/script_compiler.hpp"

namespace supple::player {

std::vector<std::filesystem::path> libraryFolders() {
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::vector<std::filesystem::path> out;
    const char* list = std::getenv("SUPPLE_SCRIPT_LIBRARY");
    std::string_view rest = list ? list : "";
    while (!rest.empty()) {
        const size_t end = std::min(rest.find(kSeparator), rest.size());
        if (end > 0) out.push_back(files::fromUtf8(rest.substr(0, end)));
        rest.remove_prefix(std::min(end + 1, rest.size()));
    }
    return out;
}

bool readScript(const std::filesystem::path& file, std::string& text) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return text.size() <= kMaxScriptBytes;
}

std::vector<std::filesystem::path> libraryScripts(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::filesystem::path script = it->path() / (it->path().filename().string() + ".cpp");
        if (std::filesystem::is_regular_file(script, ec)) out.push_back(script);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::shared_ptr<const ScriptDefinition> compileScript(const std::filesystem::path& file,
                                                      const std::string& source,
                                                      std::string* problems) {
    scriptc::Options options;
    options.interpret = std::getenv("SUPPLE_SCRIPT_INTERPRET") != nullptr;
    scriptc::Result result = scriptc::compile(source, options);
    if (!result.program) {
        if (problems)
            *problems = scriptc::toText(result.diagnostics, files::toUtf8(file.filename()));
        return nullptr;
    }
    return makeScriptDefinition(std::move(result.program), file, source, problems);
}

namespace {

class Library final : public Catalog {
  public:
    std::vector<DefinitionPtr> list() override {
        std::vector<DefinitionPtr> out;
        std::set<std::string> ids;
        std::vector<std::filesystem::path> scripts;
        for (const auto& folder : libraryFolders())
            for (auto& file : libraryScripts(folder)) scripts.push_back(std::move(file));
        for (const auto& file : scripts) {
            std::string source;
            if (!readScript(file, source)) continue;
            Entry& entry = cache_[file];
            if (!entry.definition || entry.source != source) {
                entry.source = source;
                entry.definition = compileScript(file, source, nullptr);
            }
            if (entry.definition && ids.insert(entry.definition->info().id).second)
                out.push_back(entry.definition);
        }
        return out;
    }

  private:
    // The same Definition while its script says the same: hosts list the
    // catalog often, and compiling is not free.
    struct Entry {
        std::string source;
        DefinitionPtr definition;
    };
    std::map<std::filesystem::path, Entry> cache_;
};

}  // namespace

}  // namespace supple::player

std::unique_ptr<supple::Catalog> product::makeCatalog() {
    return std::make_unique<supple::player::Library>();
}
