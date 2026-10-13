// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The player as a host sees it, in process: scripts listed as plugins, the
// semantic-params extension, the patch in the state, and scripts reloaded
// while they play.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "Check.h"
#include "plugin/Entry.h"
#include "supple/clap/semantic_params.h"
#include "supple/suite/json.h"

namespace json = supple::suite::json;

namespace {

struct Host {
    clap_host_t clap{};
    int restarts = 0, rescanAll = 0, semanticChanges = 0;
    bool callback = false;
    std::vector<clap_id> timers;
    std::vector<std::string> logged;

    Host() {
        clap.clap_version = CLAP_VERSION;
        clap.host_data = this;
        clap.name = "player-tests";
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
            },
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {}, [](const clap_host_t*) {}};
        static const clap_host_timer_support_t timer{
            [](const clap_host_t* h, uint32_t, clap_id* id) {
                *id = static_cast<clap_id>(self(h).timers.size() + 1);
                self(h).timers.push_back(*id);
                return true;
            },
            [](const clap_host_t*, clap_id) { return true; }};
        static const clap_host_log_t log{
            [](const clap_host_t* h, clap_log_severity, const char* text) {
                self(h).logged.push_back(text ? text : "");
            }};
        static const clap_host_supple_semantic_params_t semantic{
            sizeof(clap_host_supple_semantic_params_t),
            [](const clap_host_t* h, uint32_t) { self(h).semanticChanges++; }};
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timer;
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
        if (!std::strcmp(id, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS)) return &semantic;
        return nullptr;
    }
    bool loggedContaining(const char* text) const {
        for (const std::string& line : logged)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }
};

const clap_plugin_factory_t* factory() {
    return static_cast<const clap_plugin_factory_t*>(
        supple_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
}

struct Text {
    std::string text;
    clap_ostream_t out{this, [](const clap_ostream_t* s, const void* data, uint64_t n) -> int64_t {
                           static_cast<Text*>(s->ctx)->text.append(static_cast<const char*>(data),
                                                                   n);
                           return static_cast<int64_t>(n);
                       }};
    size_t read = 0;
    clap_istream_t in{this, [](const clap_istream_t* s, void* data, uint64_t n) -> int64_t {
                          auto* t = static_cast<Text*>(s->ctx);
                          const size_t take =
                              std::min<size_t>(static_cast<size_t>(n), t->text.size() - t->read);
                          std::memcpy(data, t->text.data() + t->read, take);
                          t->read += take;
                          return static_cast<int64_t>(take);
                      }};
};

struct Instance {
    Host host;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_supple_semantic_params_t* semantic = nullptr;
    bool active = false;

    explicit Instance(const char* id) {
        plugin = factory()->create_plugin(factory(), &host.clap, id);
        if (!plugin) return;
        plugin->init(plugin);
        params = static_cast<const clap_plugin_params_t*>(
            plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        state =
            static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        semantic = static_cast<const clap_plugin_supple_semantic_params_t*>(
            plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
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
    void tick(int n) {
        const auto* timer = static_cast<const clap_plugin_timer_support_t*>(
            plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
        for (int i = 0; i < n && timer && !host.timers.empty(); ++i)
            timer->on_timer(plugin, host.timers.back());
    }
    std::string schema() const {
        Text t;
        semantic->get_schema(plugin, &t.out);
        return t.text;
    }
    std::string patch(bool changed = false) const {
        Text t;
        semantic->get_patch(
            plugin, changed ? CLAP_SUPPLE_PATCH_DETAIL_CHANGED : CLAP_SUPPLE_PATCH_DETAIL_FULL,
            &t.out);
        return t.text;
    }
    bool setPatch(uint32_t mode, const std::string& doc, std::string* errors = nullptr) {
        Text e;
        const bool ok = semantic->set_patch(plugin, mode, doc.data(),
                                            static_cast<uint32_t>(doc.size()), &e.out);
        if (errors) *errors = e.text;
        return ok;
    }
    std::string describe() const {
        Text t;
        semantic->describe(plugin, CLAP_SUPPLE_PATCH_DETAIL_CHANGED, &t.out);
        return t.text;
    }
    double clapValue(const char* name) const {
        for (uint32_t i = 0; i < params->count(plugin); ++i) {
            clap_param_info_t info{};
            params->get_info(plugin, i, &info);
            if (std::strcmp(info.name, name) != 0) continue;
            double v = 0.0;
            params->get_value(plugin, info.id, &v);
            return v;
        }
        return std::nan("");
    }
    // One stereo block of a constant through process(); the left output's last sample.
    float play(float in = 0.5f) {
        float left[64], right[64];
        std::fill(left, left + 64, in);
        std::fill(right, right + 64, in);
        float* channels[] = {left, right};
        clap_audio_buffer_t buffer{channels, nullptr, 2, 0, 0};
        const clap_input_events_t events{
            nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
            [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                return nullptr;
            }};
        const clap_output_events_t out{
            nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        clap_process_t p{};
        p.frames_count = 64;
        p.audio_inputs = &buffer;
        p.audio_outputs = &buffer;
        p.audio_inputs_count = p.audio_outputs_count = 1;
        p.in_events = &events;
        p.out_events = &out;
        p.steady_time = -1;
        plugin->process(plugin, &p);
        return left[63];
    }
};

json::Value parsed(const std::string& text) {
    json::Value v;
    json::Error e;
    CHECK_MSG(json::parse(text, v, e), "%s", e.message.c_str());
    return v;
}

// A copy of a library script in a folder of its own, set as the library
// while it lives.
struct Library {
    std::filesystem::path root;
    std::string previous;
    explicit Library(const char* script) {
        const char* given = std::getenv("SUPPLE_SCRIPT_LIBRARY");
        previous = given ? given : "";
        root = std::filesystem::temp_directory_path() /
               ("player-library-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / script);
        // The script from whichever folder of the library has it.
        std::string_view rest = previous;
        while (!rest.empty()) {
            const size_t end = std::min(rest.find(':'), rest.size());
            const auto file = std::filesystem::path(rest.substr(0, end)) / script /
                              (std::string(script) + ".cpp");
            if (std::filesystem::exists(file)) {
                std::filesystem::copy_file(file, root / script / (std::string(script) + ".cpp"));
                break;
            }
            rest.remove_prefix(std::min(end + 1, rest.size()));
        }
        setenv("SUPPLE_SCRIPT_LIBRARY", root.string().c_str(), 1);
    }
    ~Library() {
        setenv("SUPPLE_SCRIPT_LIBRARY", previous.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    // Rewrites the script with `from` replaced by `to`, stamped `later` seconds on.
    void edit(const char* script, const std::string& from, const std::string& to, int later) const {
        const auto file = root / script / (std::string(script) + ".cpp");
        std::ifstream in(file);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) text.replace(at, from.size(), to);
        std::ofstream(file) << text;
        std::filesystem::last_write_time(
            file, std::filesystem::last_write_time(file) + std::chrono::seconds(later));
    }
};

}  // namespace

TEST(each_script_in_the_library_is_a_plugin) {
    CHECK(factory()->get_plugin_count(factory()) == 3);
    Instance tilt("com.example.tilt");
    CHECK(tilt.plugin != nullptr && tilt.semantic != nullptr);
    CHECK(tilt.params->count(tilt.plugin) == 3);
    CHECK(std::string(tilt.plugin->desc->name) == "Tilt");
    Instance pluck("com.example.pluck");
    CHECK(pluck.plugin != nullptr && std::string(pluck.plugin->desc->features[0]) == "instrument");
}

TEST(the_schema_names_every_clap_parameter_by_key) {
    Instance tilt("com.example.tilt");
    const json::Value schema = parsed(tilt.schema());
    CHECK(schema.find("patch_type")->asString() == "supple.tilt-patch/1.0");
    const json::Value& params = *schema.find("params");
    CHECK(params.size() == 3);
    for (uint32_t i = 0; i < tilt.params->count(tilt.plugin); ++i) {
        clap_param_info_t info{};
        tilt.params->get_info(tilt.plugin, i, &info);
        bool found = false;
        for (const json::Member& m : params.asObject())
            found = found || m.second.find("id")->asInt() == static_cast<long long>(info.id);
        CHECK(found);
        // ADR-0010: a float's CLAP value is its 0..1 position.
        CHECK(info.min_value == 0.0 && info.max_value == 1.0);
    }
    CHECK(params.find("pivot")->find("curve")->asString() == "log");
}

TEST(patches_set_merge_validate_and_describe_through_the_extension) {
    Instance tilt("com.example.tilt");
    std::string errors;
    CHECK(tilt.setPatch(CLAP_SUPPLE_SET_MERGE, R"({"params": {"tilt": 3, "pivot": "2 kHz"}})",
                        &errors));
    CHECK(std::fabs(tilt.clapValue("Tilt") - 0.75) < 1e-12);
    const json::Value patch = parsed(tilt.patch(true));
    CHECK(patch.find("params")->find("tilt")->asDouble() == 3.0);
    CHECK(std::fabs(patch.find("params")->find("pivot")->asDouble() - 2000.0) < 1e-6);
    CHECK(patch.find("params")->find("output") == nullptr);  // unchanged
    CHECK(tilt.describe().find("Tilt +3.0 dB") != std::string::npos);
    CHECK(tilt.host.semanticChanges >= 1);
    // A bad value changes nothing, and says where and how.
    const std::string before = tilt.patch();
    CHECK(!tilt.setPatch(CLAP_SUPPLE_SET_MERGE, R"({"params": {"tilt": 30}})", &errors));
    CHECK(tilt.patch() == before);
    const json::Value list = parsed(errors);
    CHECK(list.asArray().at(0).find("path")->asString() == "/params/tilt");
    CHECK(!list.asArray().at(0).find("hint")->asString().empty());
    CHECK(tilt.setPatch(CLAP_SUPPLE_SET_VALIDATE_ONLY, R"({"params": {"tilt": -6}})"));
    CHECK(tilt.patch() == before);
    // A replacing patch defaults what it leaves out.
    CHECK(tilt.setPatch(CLAP_SUPPLE_SET_REPLACE,
                        R"({"schema": "supple.tilt-patch/1.0", "name": "Flat"})"));
    CHECK(parsed(tilt.patch()).find("name")->asString() == "Flat");
    CHECK(std::fabs(tilt.clapValue("Tilt") - 0.5) < 1e-12);
}

TEST(the_state_carries_the_patch_and_restores_it) {
    Text saved;
    {
        Instance a("com.example.tilt");
        CHECK(a.setPatch(CLAP_SUPPLE_SET_MERGE,
                         R"({"name": "Warm", "params": {"tilt": -2.5, "output": -1}})"));
        CHECK(a.state->save(a.plugin, &saved.out));
    }
    Instance b("com.example.tilt");
    CHECK(b.state->load(b.plugin, &saved.in));
    const json::Value patch = parsed(b.patch(true));
    CHECK(patch.find("name")->asString() == "Warm");
    CHECK(patch.find("params")->find("tilt")->asDouble() == -2.5);
    CHECK(patch.find("params")->find("output")->asDouble() == -1.0);
}

TEST(a_changed_script_replaces_the_playing_one_in_place) {
    Library library("smoke");
    Instance a("com.suppleaudio.script-player");
    a.activate();
    for (int i = 0; i < 4; ++i) a.play();
    CHECK(std::fabs(a.play() - 0.5f) < 1e-6f);
    library.edit("smoke", "io[c][i] *= dbToGain(gain.at(i));",
                 "io[c][i] *= 0.5 * dbToGain(gain.at(i));", 2);
    a.tick(15);
    CHECK(a.host.restarts == 0);
    for (int i = 0; i < 20; ++i) a.play();  // past the crossfade
    CHECK(std::fabs(a.play() - 0.25f) < 1e-6f);
    // One that does not compile leaves the new one playing, and says why.
    library.edit("smoke", "0.5 * dbToGain", "0.5 * undefinedThing", 4);
    a.tick(15);
    CHECK(a.host.loggedContaining("not reloaded"));
    CHECK(std::fabs(a.play() - 0.25f) < 1e-6f);
}

TEST(a_script_whose_parameters_change_redefines_the_plugin) {
    Library library("smoke");
    Instance a("com.suppleaudio.script-player");
    a.activate();
    CHECK(a.setPatch(CLAP_SUPPLE_SET_MERGE, R"({"params": {"gain": -6}})"));
    library.edit("smoke", "Param peak",
                 "Param trim = decibels(\"Trim\", -6, 6, 0);\n    Param peak", 2);
    a.tick(15);
    CHECK(a.host.restarts == 1);
    a.deactivate();
    CHECK(a.host.rescanAll == 1);
    CHECK(a.params->count(a.plugin) == 3);
    const json::Value patch = parsed(a.patch());
    CHECK(patch.find("params")->find("gain")->asDouble() == -6.0);  // by key
    CHECK(patch.find("params")->find("trim")->asDouble() == 0.0);
    CHECK(a.host.semanticChanges >= 2);
}

SUPPLE_TEST_MAIN
