// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Stages as a host sees it, in process: plugins listed from files, a layer
// that keeps values by name, and an instance redefined when its file changes.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "Check.h"
#include "ProductInfo.h"
#include "plugin/Entry.h"

namespace {

struct Host {
    clap_host_t clap{};
    int restarts = 0, rescanAll = 0, rescanValues = 0;
    bool callback = false;
    std::vector<clap_id> timers;

    Host() {
        clap.clap_version = CLAP_VERSION;
        clap.host_data = this;
        clap.name = "stages-tests";
        clap.vendor = clap.url = clap.version = "";
        clap.get_extension = extension;
        clap.request_restart = [](const clap_host_t* h) { self(h).restarts++; };
        clap.request_process = [](const clap_host_t*) {};
        clap.request_callback = [](const clap_host_t* h) { self(h).callback = true; };
    }
    static Host& self(const clap_host_t* h) { return *static_cast<Host*>(h->host_data); }
    static const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
        static const clap_host_params_t params{
            [](const clap_host_t* h, clap_param_rescan_flags flags) {
                if (flags & CLAP_PARAM_RESCAN_ALL) self(h).rescanAll++;
                if (flags & CLAP_PARAM_RESCAN_VALUES) self(h).rescanValues++;
            },
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {}, [](const clap_host_t*) {}};
        static const clap_host_timer_support_t timer{
            [](const clap_host_t* h, uint32_t, clap_id* id) {
                *id = static_cast<clap_id>(self(h).timers.size() + 1);
                self(h).timers.push_back(*id);
                return true;
            },
            [](const clap_host_t*, clap_id) { return true; }};
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timer;
        return nullptr;
    }
};

const clap_plugin_factory_t* factory() {
    return static_cast<const clap_plugin_factory_t*>(supple_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

struct MemoryStream {
    std::vector<uint8_t> bytes;
    size_t read = 0;
    clap_ostream_t out{this, [](const clap_ostream_t* s, const void* data, uint64_t n) -> int64_t {
                           auto* m = static_cast<MemoryStream*>(s->ctx);
                           m->bytes.insert(m->bytes.end(), static_cast<const uint8_t*>(data),
                                           static_cast<const uint8_t*>(data) + n);
                           return static_cast<int64_t>(n);
                       }};
    clap_istream_t in{this, [](const clap_istream_t* s, void* data, uint64_t n) -> int64_t {
                          auto* m = static_cast<MemoryStream*>(s->ctx);
                          const size_t take = std::min<size_t>(static_cast<size_t>(n), m->bytes.size() - m->read);
                          std::memcpy(data, m->bytes.data() + m->read, take);
                          m->read += take;
                          return static_cast<int64_t>(take);
                      }};
};

struct Instance {
    Host host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    bool active = false;

    explicit Instance(const std::string& id) {
        plugin = factory()->create_plugin(factory(), &host.clap, id.c_str());
        if (!plugin) return;
        plugin->init(plugin);
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    }
    ~Instance() {
        if (!plugin) return;
        if (active) plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    void activate() { active = plugin->activate(plugin, 48000.0, 1, 256); }
    void deactivate() {
        plugin->deactivate(plugin);
        active = false;
    }
    // Ticks the layer's timer (the last registered) `n` times.
    void tick(int n) {
        const auto* timer =
            static_cast<const clap_plugin_timer_support_t*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
        for (int i = 0; i < n && timer && !host.timers.empty(); ++i) timer->on_timer(plugin, host.timers.back());
    }
    uint32_t count() const { return params->count(plugin); }
    // The value of the parameter named `name`, or NaN.
    double value(const char* name) const {
        for (uint32_t i = 0; i < count(); ++i) {
            clap_param_info_t info{};
            params->get_info(plugin, i, &info);
            if (std::strcmp(info.name, name) != 0) continue;
            double v = 0.0;
            params->get_value(plugin, info.id, &v);
            return v;
        }
        return std::nan("");
    }
    // Sets it as host automation would, through a flush.
    void set(const char* name, double v) {
        for (uint32_t i = 0; i < count(); ++i) {
            clap_param_info_t info{};
            params->get_info(plugin, i, &info);
            if (std::strcmp(info.name, name) != 0) continue;
            clap_event_param_value_t e{};
            e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            e.param_id = info.id;
            e.note_id = e.port_index = e.channel = e.key = -1;
            e.value = v;
            const clap_input_events_t in{&e, [](const clap_input_events_t*) -> uint32_t { return 1; },
                                         [](const clap_input_events_t* l, uint32_t) {
                                             return &static_cast<const clap_event_param_value_t*>(l->ctx)->header;
                                         }};
            const clap_output_events_t out{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) {
                                               return true;
                                           }};
            params->flush(plugin, &in, &out);
        }
    }
    uint32_t stagesExtension() const {
        struct Ext {
            uint32_t(CLAP_ABI* count)(const clap_plugin_t*);
        };
        const auto* ext = static_cast<const Ext*>(plugin->get_extension(plugin, "com.suppleaudio.stages/1"));
        return ext ? ext->count(plugin) : 0;
    }
};

// A folder of plugin files, set as SUPPLE_STAGES_DIR while it lives.
struct Folder {
    std::filesystem::path root;
    Folder() {
        root = std::filesystem::temp_directory_path() /
               ("stages-plugin-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        setDir(root.string());
    }
    ~Folder() {
        setDir("");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    // Writes a file, its time stamped `later` seconds on so a reader sees it changed.
    void write(const std::string& name, const std::string& text, int later = 0) const {
        const auto p = root / name;
        std::ofstream(p) << text;
        std::filesystem::last_write_time(p, std::filesystem::last_write_time(p) + std::chrono::seconds(later));
    }
    static void setDir(const std::string& dir) {
#ifdef _WIN32
        _putenv_s("SUPPLE_STAGES_DIR", dir.c_str());
#else
        if (dir.empty()) unsetenv("SUPPLE_STAGES_DIR");
        else setenv("SUPPLE_STAGES_DIR", dir.c_str(), 1);
#endif
    }
};

const std::string kDuo = std::string(product::info::kId) + ".duo";

}  // namespace

TEST(the_catalog_reaches_hosts_through_the_factory) {
    Folder folder;
    folder.write("duo.stages", "Low\nHigh\n");
    CHECK(factory()->get_plugin_count(factory()) == 2);
    const clap_plugin_descriptor_t* builtIn = factory()->get_plugin_descriptor(factory(), 0);
    const clap_plugin_descriptor_t* duo = factory()->get_plugin_descriptor(factory(), 1);
    CHECK(builtIn && std::strcmp(builtIn->id, product::info::kId) == 0);
    CHECK(duo && duo->id == kDuo && std::string(duo->name) == std::string(product::info::kName) + " duo");
    Instance a(kDuo);
    CHECK(a.plugin && a.count() == 3 && a.stagesExtension() == 2);
    CHECK(Instance("com.example.nothing").plugin == nullptr);
}

TEST(state_keeps_each_stage_value_by_name_when_the_stages_move) {
    Folder folder;
    folder.write("duo.stages", "Low\nHigh\n");
    MemoryStream saved;
    {
        Instance a(kDuo);
        a.set("High", -6.0);
        a.set("Low", 3.0);
        CHECK(a.value("High") == -6.0);
        CHECK(a.state->save(a.plugin, &saved.out));
    }
    // The file reordered: High is now the first stage, with Low's old ID.
    folder.write("duo.stages", "High\nLow\n", 2);
    Instance b(kDuo);
    CHECK(b.state->load(b.plugin, &saved.in));
    CHECK(b.value("High") == -6.0 && b.value("Low") == 3.0);
}

TEST(a_changed_file_redefines_an_active_instance_at_its_restart) {
    Folder folder;
    folder.write("duo.stages", "Low\nHigh\n");
    Instance a(kDuo);
    a.activate();
    a.set("High", -6.0);
    folder.write("duo.stages", "Low\nMid\nHigh\n", 2);
    a.tick(15);
    CHECK(a.host.restarts == 1);
    CHECK(a.count() == 3);  // nothing changes while active
    a.deactivate();
    CHECK(a.host.rescanAll == 1);
    CHECK(a.count() == 4 && a.stagesExtension() == 3);
    CHECK(a.value("High") == -6.0 && a.value("Mid") == 0.0);
    a.activate();
    CHECK(a.active);
}

TEST(an_inactive_instance_is_redefined_at_once) {
    Folder folder;
    folder.write("duo.stages", "Low\nHigh\n");
    Instance a(kDuo);
    a.set("Low", 1.5);
    folder.write("duo.stages", "Low\n", 2);
    a.tick(15);
    CHECK(a.host.restarts == 0 && a.host.rescanAll == 1);
    CHECK(a.count() == 2 && a.value("Low") == 1.5 && std::isnan(a.value("High")));
    // A file the catalog would refuse leaves the instance as it is.
    folder.write("duo.stages", "Low\nLow\n", 4);
    a.tick(15);
    CHECK(a.count() == 2 && a.host.rescanAll == 1);
}

SUPPLE_TEST_MAIN
