// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <fstream>
#include "chrome.h"
#include "help_support.h"
#include "supple/product.h"
#include "test_support.h"

using namespace supple;
namespace fs = std::filesystem;
namespace help = supple::gui::help;
namespace {
struct Folder {
    fs::path path =
        fs::temp_directory_path() / ("supple-help-test-" + std::to_string(testProcessId()));
    Folder() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Folder() { fs::remove_all(path); }
    void write(const fs::path& relative, const std::string& text = "skill") {
        fs::create_directories((path / relative).parent_path());
        std::ofstream(path / relative) << text;
    }
};
}  // namespace
TEST_CASE("Help constructs each harness registration without splitting paths") {
    const std::string server = "/a path/it's a server/$HOME-mcp";
    const std::string slug(product::kSlug);
    CHECK(help::registrationArgs(help::Harness::Claude, server) ==
          std::vector<std::string>{"mcp", "add", "-s", "user", slug, "--", server});
    CHECK(help::registrationArgs(help::Harness::Codex, server) ==
          std::vector<std::string>{"mcp", "add", slug, "--", server});
    CHECK(help::registrationArgs(help::Harness::Hermes, server) ==
          std::vector<std::string>{"mcp", "add", slug, "--command", server});
    CHECK(help::commandText("cli", {server}, help::Platform::Posix) ==
          "'cli' '/a path/it'\\''s a server/$HOME-mcp'");
    CHECK(help::commandText("C:\\a b\\cli.cmd", {"it's"}, help::Platform::Windows) ==
          "& 'C:\\a b\\cli.cmd' 'it''s'");
    const auto script = help::windowsScript("C:\\a & b\\cli.cmd", {"100% & !USER!", ""});
    REQUIRE(script);
    CHECK(script->find("DisableDelayedExpansion") != std::string::npos);
    CHECK(script->find("\"C:\\a & b\\cli.cmd\" \"100%% & !USER!\" \"\" 2>&1") != std::string::npos);
    CHECK_FALSE(help::windowsScript("cli", {"bad\"argument"}));
    CHECK_FALSE(help::windowsScript("cli", {"bad\nargument"}));
}
TEST_CASE("Help locates adjacent installed and build-tree helper executables") {
    Folder f;
    const std::string slug(product::kSlug);
    for (const auto platform : {help::Platform::Posix, help::Platform::Windows}) {
        const std::string suffix = platform == help::Platform::Windows ? ".exe" : "";
        for (const auto& relative :
             {fs::path("prefix/bin"), fs::path("flat"), fs::path("App.app/Contents/MacOS")}) {
            const auto dir = f.path / relative;
            const auto helpers = help::executables(dir / (slug + suffix), platform);
            CHECK(helpers.mcp == dir / (slug + "-mcp" + suffix));
            CHECK(helpers.engine == dir / (slug + "-engine" + suffix));
        }
        const auto dir = f.path / "build/dev/daw/gui";
        const auto helpers = help::executables(dir / (slug + suffix), platform);
        CHECK(helpers.mcp == f.path / "build/dev/mcp" / (slug + "-mcp" + suffix));
        CHECK(helpers.engine == f.path / "build/dev/host/engine" / (slug + "-engine" + suffix));
        f.write(fs::path("build/dev/daw/gui") / (slug + "-mcp" + suffix));
        CHECK(help::executables(dir / (slug + suffix), platform).mcp ==
              dir / (slug + "-mcp" + suffix));
    }
}
TEST_CASE("Help skill copy and link replace only skill folders or links with consent") {
    Folder f;
    f.write("source/SKILL.md", "original");
    f.write("source/guides/guide.md", "guide");
    fs::create_directory(f.path / "destination");
    const auto source = f.path / "source";
    const auto parent = f.path / "destination";
    const auto target = parent / std::string(product::kSlug);
    CHECK(help::inspectTarget(target) == help::Target::Missing);
    REQUIRE(help::exportSkill(source, parent, false, false));
    CHECK(fs::is_regular_file(target / "SKILL.md"));
    CHECK(fs::is_regular_file(target / "guides/guide.md"));
    CHECK(help::inspectTarget(target) == help::Target::Skill);
    CHECK_FALSE(help::exportSkill(source, parent, false, false));
    f.write(fs::path("destination") / std::string(product::kSlug) / "old.txt");
    f.write("external/SKILL.md", "keep external");
    REQUIRE(help::exportSkill(f.path / "external", target, true, false));
    REQUIRE(help::exportSkill(source, parent, true, true));
    CHECK(help::inspectTarget(target) == help::Target::Link);
    CHECK(fs::equivalent(source, target));
    CHECK_FALSE(fs::exists(target / "old.txt"));
    CHECK(fs::is_regular_file(f.path / "external/SKILL.md"));
    REQUIRE(help::exportSkill(source, parent, false, true));
    CHECK(help::inspectTarget(target) == help::Target::Skill);
    CHECK(fs::exists(source / "SKILL.md"));  // removing the old link did not follow it
    fs::remove_all(target);
    fs::create_directory(target);
    std::ofstream(target / "unrelated.txt") << "keep";
    CHECK(help::inspectTarget(target) == help::Target::Refuse);
    CHECK_FALSE(help::exportSkill(source, parent, false, true));
    CHECK(fs::exists(target / "unrelated.txt"));
    fs::remove_all(target);
    std::ofstream(target) << "keep";
    CHECK_FALSE(help::exportSkill(source, parent, true, true));
    CHECK(fs::is_regular_file(target));
    CHECK_FALSE(help::exportSkill(source, source, false, false));
    CHECK_FALSE(help::exportSkill(f.path / "missing", parent, false, true));
    CHECK(fs::is_regular_file(target));
}
TEST_CASE("Help refuses to replace the source skill or a containing directory") {
    Folder f;
    const fs::path slug{std::string(product::kSlug)};
    f.write(slug / "SKILL.md");
    CHECK_FALSE(help::exportSkill(f.path / slug, f.path, false, true));
    f.write(slug / "nested/SKILL.md");
    CHECK_FALSE(help::exportSkill(f.path / slug / "nested", f.path, false, true));
    CHECK(fs::exists(f.path / slug / "nested/SKILL.md"));
}
TEST_CASE("Help is appended to saved pre-Help menus without resetting customizations") {
    Folder f;
    const auto path = f.path / "menus.json";
    gui::ChromeLayout old;
    old.menus = {{"Custom", {"ui.actions"}}};
    old.toolbar = {"ui.actions"};
    REQUIRE(old.save(path));
    auto loaded = gui::ChromeLayout::load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->menus.size() == 2);
    CHECK(loaded->menus[0].title == "Custom");
    CHECK(loaded->menus.back().title == "Help");
    CHECK(loaded->menus.back().items == gui::ChromeLayout::defaults().menus.back().items);
    CHECK(loaded->toolbar == old.toolbar);
    REQUIRE(loaded->save(path));
    loaded = gui::ChromeLayout::load(path);
    REQUIRE(loaded);
    CHECK(loaded->menus.size() == 2);
}
TEST_CASE("Help captures stdout stderr and exit status without evaluating arguments") {
#if !defined(_WIN32)
    const auto result = help::runCaptured(
        "/bin/sh", {"-c", "printf '%s' \"$1\"; printf error >&2; exit 7", "probe", "a ' b; $HOME"});
    REQUIRE(result);
    CHECK(result->exitCode == 7);
    CHECK(result->text == "a ' b; $HOMEerror");
#else
    Folder f;
    const auto shim = f.path / "fake harness.cmd";
    std::ofstream(shim) << "@echo off\r\n@echo stdout %1\r\n@echo stderr >&2\r\n@exit /b 7\r\n";
    const auto result = help::runCaptured(shim, {"spaces & 100% !literal!"});
    REQUIRE(result);
    CHECK(result->exitCode == 7);
    CHECK(result->text.find("spaces & 100% !literal!") != std::string::npos);
    CHECK(result->text.find("stderr") != std::string::npos);
#endif
}

#if !defined(_WIN32)
TEST_CASE("Help protects an existing Codex registration and accepts Hermes tool discovery") {
    Folder f;
    const auto cli = f.path / "harness";
    const auto marker = f.path / "added";
    std::ofstream(cli) << "#!/bin/sh\nif [ \"$2\" = get ]; then echo '{\"command\":\"original\"}'; "
                          "exit 0; fi\ntouch '"
                       << marker.string() << "'\n";
    fs::permissions(cli, fs::perms::owner_all);
    auto result = help::registerHarness(help::Harness::Codex, cli, "/tmp/new-mcp");
    REQUIRE(result);
    CHECK(result->text.find("already exists") != std::string::npos);
    CHECK_FALSE(fs::exists(marker));
    std::ofstream(cli) << "#!/bin/sh\nif [ \"$2\" = get ]; then echo 'Error: No MCP server named "
                          "test found.' >&2; exit 1; fi\ntouch '"
                       << marker.string() << "'\n";
    result = help::registerHarness(help::Harness::Codex, cli, "/tmp/new-mcp");
    REQUIRE(result);
    CHECK(fs::exists(marker));
    std::ofstream(cli) << "#!/bin/sh\nIFS= read -r answer || exit 9\n[ -z \"$answer\" ] || exit "
                          "8\necho 'Saved to config'\n";
    result = help::registerHarness(help::Harness::Hermes, cli, "/tmp/new-mcp");
    REQUIRE(result);
    CHECK(result->exitCode == 0);
    CHECK(result->text.find("Saved to config") != std::string::npos);
}
TEST_CASE("Help cancels and reaps a running child") {
    const auto result = help::runCaptured("/bin/sh", {"-c", "sleep 10"}, [] { return true; });
    REQUIRE(result);
    CHECK(result->exitCode != 0);
}
#endif

#if !defined(_WIN32)
TEST_CASE("Help can replace a dangling link but rejects a parent alias into the source") {
    Folder f;
    f.write("source/SKILL.md");
    fs::create_directory(f.path / "destination");
    const auto target = f.path / "destination" / std::string(product::kSlug);
    fs::create_directory_symlink(f.path / "missing", target);
    CHECK(help::inspectTarget(target) == help::Target::Link);
    CHECK_FALSE(help::exportSkill(f.path / "source", target.parent_path(), false, false));
    REQUIRE(help::exportSkill(f.path / "source", target.parent_path(), false, true));
    CHECK(help::inspectTarget(target) == help::Target::Skill);
    fs::create_directory_symlink(f.path / "source", f.path / "alias");
    CHECK_FALSE(help::exportSkill(f.path / "source", f.path / "alias", false, true));
}
#endif
