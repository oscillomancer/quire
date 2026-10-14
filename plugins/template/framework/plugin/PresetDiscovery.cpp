// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "plugin/PresetDiscovery.h"

#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "core/Catalog.h"
#include "core/Files.h"
#include "core/PresetLibrary.h"
#include "core/StateFormat.h"

namespace supple::plugin {
namespace {

// One provider per plugin in the catalog that has presets.
struct ProviderInfo {
    DefinitionPtr definition;
    std::string id, name;
    clap_preset_discovery_provider_descriptor_t clap{};

    explicit ProviderInfo(DefinitionPtr d) : definition(std::move(d)) {
        const PluginInfo& i = definition->info();
        id = i.id + ".presets";
        name = i.name + " presets";
        clap = {CLAP_VERSION_INIT, id.c_str(), name.c_str(), i.vendor.c_str()};
    }
};

// Every provider handed out, kept for the life of the module (hosts keep
// the pointers), and the catalog's as last listed (under catalogMutex()).
struct Providers {
    std::vector<std::unique_ptr<ProviderInfo>> all;
    std::vector<const ProviderInfo*> current;

    void refresh() {
        current.clear();
        for (DefinitionPtr& d : catalog().list()) {
            if (!d->info().presets) continue;
            const ProviderInfo* found = nullptr;
            for (auto& existing : all)
                if (existing->definition == d) found = existing.get();
            if (!found) {
                all.push_back(std::make_unique<ProviderInfo>(std::move(d)));
                found = all.back().get();
            }
            current.push_back(found);
        }
    }
};

Providers& providers() {
    static Providers instance;
    return instance;
}

struct Provider {
    clap_preset_discovery_provider_t clap{};
    const clap_preset_discovery_indexer_t* indexer = nullptr;
    const ProviderInfo* info = nullptr;
    std::string fileTypeName;
    std::string userFolder;  // kept for the indexer, which may hold the pointer
};

Provider* self(const clap_preset_discovery_provider_t* p) { return static_cast<Provider*>(p->provider_data); }

void describe(const Provider& s, const clap_preset_discovery_metadata_receiver_t* receiver, uint32_t flags) {
    const PluginInfo& info = s.info->definition->info();
    const clap_universal_plugin_id_t id{"clap", info.id.c_str()};
    receiver->add_plugin_id(receiver, &id);
    receiver->set_flags(receiver, flags);
    if (!info.vendor.empty()) receiver->add_creator(receiver, info.vendor.c_str());
}

bool CLAP_ABI providerInit(const clap_preset_discovery_provider_t* p) {
    Provider* s = self(p);
    const Definition& definition = *s->info->definition;
    const clap_preset_discovery_filetype_t type{s->fileTypeName.c_str(), nullptr, PresetLibrary::kExtension + 1};
    if (!s->indexer->declare_filetype(s->indexer, &type)) return false;
    if (!definition.presets().empty()) {
        const clap_preset_discovery_location_t factory{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                                                       CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
        if (!s->indexer->declare_location(s->indexer, &factory)) return false;
    }
    std::error_code ec;
    const auto folder = PresetLibrary::defaultUserFolder(definition.info());
    if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
        s->userFolder = PresetLibrary::toUtf8(folder);
        const clap_preset_discovery_location_t user{CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "User",
                                                    CLAP_PRESET_DISCOVERY_LOCATION_FILE, s->userFolder.c_str()};
        if (!s->indexer->declare_location(s->indexer, &user)) return false;
    }
    return true;
}

void CLAP_ABI providerDestroy(const clap_preset_discovery_provider_t* p) { delete self(p); }

bool CLAP_ABI providerMetadata(const clap_preset_discovery_provider_t* p, uint32_t kind, const char* location,
                               const clap_preset_discovery_metadata_receiver_t* receiver) {
    const Provider& s = *self(p);
    const Definition& definition = *s.info->definition;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        for (const FactoryPreset& preset : definition.presets()) {
            if (!receiver->begin_preset(receiver, preset.name.c_str(), preset.name.c_str())) return true;
            describe(s, receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            if (!preset.category.empty()) receiver->add_feature(receiver, preset.category.c_str());
        }
        return true;
    }
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location) return false;
    const std::filesystem::path file = PresetLibrary::fromUtf8(location);
    std::vector<uint8_t> bytes;
    state::Snapshot snapshot(definition);
    if (!files::readBounded(file, bytes, state::kMaxBytes) ||
        state::load(bytes.data(), bytes.size(), state::fourcc(definition.info().stateTag.c_str()), snapshot) !=
            state::LoadResult::Ok) {
        receiver->on_error(receiver, 0, "not a preset of this plugin");
        return false;
    }
    const std::string name = PresetLibrary::toUtf8(file.stem());
    if (!receiver->begin_preset(receiver, name.c_str(), nullptr)) return true;
    describe(s, receiver, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
    return true;
}

const void* CLAP_ABI providerExtension(const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

uint32_t CLAP_ABI factoryCount(const clap_preset_discovery_factory_t*) {
    const std::lock_guard lock(catalogMutex());
    providers().refresh();
    return static_cast<uint32_t>(providers().current.size());
}

const clap_preset_discovery_provider_descriptor_t* CLAP_ABI factoryDescriptor(const clap_preset_discovery_factory_t*,
                                                                               uint32_t index) {
    const std::lock_guard lock(catalogMutex());
    if (providers().current.empty()) providers().refresh();
    return index < providers().current.size() ? &providers().current[index]->clap : nullptr;
}

const clap_preset_discovery_provider_t* CLAP_ABI factoryCreate(const clap_preset_discovery_factory_t*,
                                                                const clap_preset_discovery_indexer_t* indexer,
                                                                const char* id) {
    if (!indexer || !id) return nullptr;
    const ProviderInfo* info = nullptr;
    {
        const std::lock_guard lock(catalogMutex());
        if (providers().current.empty()) providers().refresh();
        for (const ProviderInfo* candidate : providers().current)
            if (candidate->id == id) info = candidate;
    }
    if (!info) return nullptr;
    auto* s = new Provider();
    s->indexer = indexer;
    s->info = info;
    s->fileTypeName = info->definition->info().name + " preset";
    s->clap.desc = &info->clap;
    s->clap.provider_data = s;
    s->clap.init = providerInit;
    s->clap.destroy = providerDestroy;
    s->clap.get_metadata = providerMetadata;
    s->clap.get_extension = providerExtension;
    return &s->clap;
}

const clap_preset_discovery_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

}  // namespace

const clap_preset_discovery_factory_t* presetDiscoveryFactory() { return &kFactory; }

}  // namespace supple::plugin
