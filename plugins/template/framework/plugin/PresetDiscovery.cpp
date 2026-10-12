// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "plugin/PresetDiscovery.h"

#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "ProductInfo.h"
#include "core/Preset.h"
#include "core/Files.h"
#include "core/PresetLibrary.h"
#include "core/StateFormat.h"

namespace supple::plugin {
namespace {

namespace info = product::info;

const std::string kProviderId = std::string(info::kId) + ".presets";
const std::string kProviderName = std::string(info::kName) + " presets";
const std::string kFileTypeName = std::string(info::kName) + " preset";

const clap_preset_discovery_provider_descriptor_t kDescriptor = {CLAP_VERSION_INIT, kProviderId.c_str(),
                                                                  kProviderName.c_str(), info::kVendor};

struct Provider {
    clap_preset_discovery_provider_t clap{};
    const clap_preset_discovery_indexer_t* indexer = nullptr;
    std::string userFolder;  // kept for the indexer, which may hold the pointer
};

Provider* self(const clap_preset_discovery_provider_t* p) { return static_cast<Provider*>(p->provider_data); }

void describe(const clap_preset_discovery_metadata_receiver_t* receiver, uint32_t flags) {
    const clap_universal_plugin_id_t id{"clap", info::kId};
    receiver->add_plugin_id(receiver, &id);
    receiver->set_flags(receiver, flags);
    if (info::kVendor[0]) receiver->add_creator(receiver, info::kVendor);
}

bool CLAP_ABI providerInit(const clap_preset_discovery_provider_t* p) {
    Provider* s = self(p);
    const clap_preset_discovery_filetype_t type{kFileTypeName.c_str(), nullptr, PresetLibrary::kExtension + 1};
    if (!s->indexer->declare_filetype(s->indexer, &type)) return false;
    if (presets::kFactoryCount > 0) {
        const clap_preset_discovery_location_t factory{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                                                       CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
        if (!s->indexer->declare_location(s->indexer, &factory)) return false;
    }
    std::error_code ec;
    const auto folder = PresetLibrary::defaultUserFolder();
    if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
        s->userFolder = PresetLibrary::toUtf8(folder);
        const clap_preset_discovery_location_t user{CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "User",
                                                    CLAP_PRESET_DISCOVERY_LOCATION_FILE, s->userFolder.c_str()};
        if (!s->indexer->declare_location(s->indexer, &user)) return false;
    }
    return true;
}

void CLAP_ABI providerDestroy(const clap_preset_discovery_provider_t* p) { delete self(p); }

bool CLAP_ABI providerMetadata(const clap_preset_discovery_provider_t*, uint32_t kind, const char* location,
                               const clap_preset_discovery_metadata_receiver_t* receiver) {
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
        for (size_t i = 0; i < presets::kFactoryCount; ++i) {
            const Preset& preset = presets::factory(i);
            if (!receiver->begin_preset(receiver, preset.name, preset.name)) return true;
            describe(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
            if (preset.category[0]) receiver->add_feature(receiver, preset.category);
        }
        return true;
    }
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location) return false;
    const std::filesystem::path file = PresetLibrary::fromUtf8(location);
    std::vector<uint8_t> bytes;
    state::Snapshot snapshot;
    if (!files::readBounded(file, bytes, state::kMaxBytes) ||
        state::load(bytes.data(), bytes.size(), state::fourcc(info::kStateTag), snapshot) != state::LoadResult::Ok) {
        receiver->on_error(receiver, 0, "not a preset of this plugin");
        return false;
    }
    const std::string name = PresetLibrary::toUtf8(file.stem());
    if (!receiver->begin_preset(receiver, name.c_str(), nullptr)) return true;
    describe(receiver, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
    return true;
}

const void* CLAP_ABI providerExtension(const clap_preset_discovery_provider_t*, const char*) { return nullptr; }

uint32_t CLAP_ABI factoryCount(const clap_preset_discovery_factory_t*) { return 1; }

const clap_preset_discovery_provider_descriptor_t* CLAP_ABI factoryDescriptor(const clap_preset_discovery_factory_t*,
                                                                               uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_preset_discovery_provider_t* CLAP_ABI factoryCreate(const clap_preset_discovery_factory_t*,
                                                                const clap_preset_discovery_indexer_t* indexer,
                                                                const char* id) {
    if (!indexer || !id || std::strcmp(id, kDescriptor.id) != 0) return nullptr;
    auto* s = new Provider();
    s->indexer = indexer;
    s->clap.desc = &kDescriptor;
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
