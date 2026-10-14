// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// script.run through the sandbox's registry (see test_scripts.cpp in the
// engine for why this is its own binary).
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>

#include <supple/sandbox/commands.h>

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {
const fs::path kFixture = FIXTURE_CLAP_PATH;

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-sandbox-scripts-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<SandboxHost> makeHost(const fs::path& dir) {
    SessionOptions o;
    o.workdir = dir;
    o.pluginPath = {kFixture};
    auto s = Session::open(o);
    REQUIRE(s);
    return std::make_unique<SandboxHost>(std::move(*s));
}

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};
}  // namespace

TEST_CASE("a script runs the sandbox's commands, and its batch rolls back") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "script");
    cmd::Dispatcher d(registry, *host);
    host->attachCommands(d);

    auto loaded = d.execute(
        {"script.run",
         j(R"json({"name": "loader", "code": "return q.plugin.load{plugin = 'com.suppleaudio.test.sine'}"})json"),
         kTest});
    REQUIRE_MESSAGE(loaded, loaded.error().toString());
    CHECK((*loaded)["result"]["instance"].isString());
    CHECK(host->session().chain().size() == 1);

    // An error inside the batch takes the effect it loaded with it.
    auto rolled = d.execute(
        {"script.run",
         j(R"json({"name": "loader", "code": "q.batch('loads', function()\nq.plugin.load{plugin = 'com.suppleaudio.test.gain'}\nq.plugin.load{plugin = 'no.such.plugin'}\nend)"})json"),
         kTest});
    REQUIRE_FALSE(rolled);
    CHECK(rolled.error().code == "sandbox.unknown_plugin");
    CHECK(host->session().chain().size() == 1);
}
