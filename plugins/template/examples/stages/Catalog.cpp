// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "Catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>

#include "core/Catalog.h"
#include "core/Files.h"
#include "core/Param.h"
#include "ProductInfo.h"

namespace product {

supple::DefinitionPtr makeStages(std::string id, std::string name, std::vector<std::string> stages,
                                 std::filesystem::path file) {
    supple::PluginInfo p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.vendor = info::kVendor;
    p.url = info::kUrl;
    p.version = info::kVersion;
    p.description = info::kDescription;
    p.features = {"audio-effect", "utility"};
    p.stateTag = info::kStateTag;
    p.layouts = {{2, supple::Layout::Stereo}, {1, supple::Layout::Mono}};

    supple::ParamTable::Builder b;
    b.add(supple::decibels(kOutput, "Output", -24.0, 24.0, 0.0).help("The level after every stage."));
    for (size_t k = 0; k < stages.size(); ++k)
        b.add(supple::decibels(kStage0 + static_cast<uint32_t>(k), b.text(stages[k]), -24.0, 24.0, 0.0)
                  .in("Stages")
                  .help(b.text("Stage " + std::to_string(k + 1) + "'s gain.")));
    return std::make_shared<StagesDefinition>(std::move(p), b.build(), std::move(stages), std::move(file));
}

std::vector<std::string> readStages(const std::filesystem::path& file) {
    std::vector<std::string> stages;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line) && stages.size() < static_cast<size_t>(kMaxStages);) {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        const auto last = line.find_last_not_of(" \t\r");
        stages.push_back(line.substr(first, last - first + 1).substr(0, 31));
    }
    return stages;
}

namespace {

class StagesCatalog final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        std::vector<supple::DefinitionPtr> out{builtIn_};
        const char* folder = std::getenv("SUPPLE_STAGES_DIR");
        if (!folder || !*folder) return out;
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(supple::files::fromUtf8(folder), ec), end; !ec && it != end;
             it.increment(ec))
            if (it->path().extension() == ".stages") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const auto& file : files)
            if (supple::DefinitionPtr d = load(file)) out.push_back(std::move(d));
        return out;
    }

  private:
    // The same Definition for as long as its file says the same: hosts list
    // a catalog often, and a plugin is new only when it changed.
    supple::DefinitionPtr load(const std::filesystem::path& file) {
        std::vector<std::string> stages = readStages(file);
        std::string stem = supple::files::toUtf8(file.stem());
        std::string key = stem;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool valid = !stages.empty() && !key.empty();
        for (char c : key) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '-');
        if (!valid) return nullptr;
        auto& cached = cache_[file];
        const auto* known = cached ? static_cast<const StagesDefinition*>(cached.get()) : nullptr;
        if (!known || known->stages() != stages)
            cached = makeStages(std::string(info::kId) + "." + key, std::string(info::kName) + " " + stem,
                                std::move(stages), file);
        return cached->problems().empty() ? cached : nullptr;
    }

    const supple::DefinitionPtr builtIn_ = makeStages(info::kId, info::kName, {"Low", "High"});
    std::map<std::filesystem::path, supple::DefinitionPtr> cache_;
};

}  // namespace

std::unique_ptr<supple::Catalog> makeCatalog() { return std::make_unique<StagesCatalog>(); }

}  // namespace product
