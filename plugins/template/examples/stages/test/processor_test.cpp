// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Stages' plugins, made as the catalog makes them and run through the
// Engine the plugin uses.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Catalog.h"
#include "Check.h"
#include "Processor.h"
#include "core/Catalog.h"
#include "core/Engine.h"

using namespace supple;
using namespace product;

namespace {

using Engine = BasicEngine<Processor>;

// One block of a constant through an activated engine; what comes out.
double through(Engine& engine, double in) {
    std::vector<double> left(64, in), right(64, in);
    double* io[] = {left.data(), right.data()};
    for (int i = 0; i < 1000; ++i) {  // until the 20 ms glide settles
        std::fill(left.begin(), left.end(), in);
        std::fill(right.begin(), right.end(), in);
        engine.process(io, io, 64);
    }
    return left.back();
}

void setDb(Engine& engine, uint32_t id, double db) {
    const ParamTable& t = engine.definition()->params();
    const int i = t.indexOf(id);
    engine.store().set(static_cast<size_t>(i), toHost(t.at(static_cast<size_t>(i)), db));
}

struct Folder {
    std::filesystem::path root;
    Folder() {
        root = std::filesystem::temp_directory_path() /
               ("stages-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    ~Folder() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const auto p = root / name;
        std::ofstream(p) << text;
        return p;
    }
};

}  // namespace

TEST(each_stage_multiplies_and_the_output_follows) {
    const DefinitionPtr d = makeStages("com.example.stages", "Stages", {"A", "B", "C"});
    CHECK(d->problems().empty());
    CHECK(d->params().count() == 4);
    Engine engine(d);
    CHECK(engine.processor().stages() == 3);
    engine.activate(48000.0, 64, 2, 2);
    CHECK_NEAR(through(engine, 0.5), 0.5, 1e-12);  // every stage at 0 dB
    setDb(engine, kStage0 + 0, 6.0);
    setDb(engine, kStage0 + 2, -12.0);
    setDb(engine, kOutput, 3.0);
    CHECK_NEAR(through(engine, 0.5), 0.5 * std::pow(10.0, (6.0 - 12.0 + 3.0) / 20.0), 1e-9);
}

TEST(a_plugin_file_names_its_stages_and_a_bad_one_is_not_offered) {
    Folder folder;
    const auto file = folder.write("Wide.stages", "  Body \n\nAir\r\n");
    CHECK(readStages(file) == (std::vector<std::string>{"Body", "Air"}));
    folder.write("twice.stages", "Same\nSame\n");   // a stage named twice
    folder.write("output.stages", "Output\n");      // the Output level's name
    folder.write("ignored.txt", "Low\n");
#ifdef _WIN32
    _putenv_s("SUPPLE_STAGES_DIR", folder.root.string().c_str());
#else
    setenv("SUPPLE_STAGES_DIR", folder.root.string().c_str(), 1);
#endif
    const std::vector<DefinitionPtr> plugins = catalog().list();
    CHECK(plugins.size() == 2);
    CHECK(plugins[0]->info().id == info::kId && plugins[0]->params().count() == 3);
    CHECK(plugins[1]->info().id == std::string(info::kId) + ".wide");
    CHECK(plugins[1]->info().name == std::string(info::kName) + " Wide");
    // Listed again unchanged, the same plugin: a host's descriptor stays put.
    CHECK(catalog().list()[1] == plugins[1]);
    CHECK(catalog().find(std::string(info::kId) + ".twice") == nullptr);
#ifdef _WIN32
    _putenv_s("SUPPLE_STAGES_DIR", "");
#else
    unsetenv("SUPPLE_STAGES_DIR");
#endif
}

SUPPLE_TEST_MAIN
