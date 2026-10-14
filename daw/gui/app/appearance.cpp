// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "appearance.h"

#include <cstdlib>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::gui {

std::filesystem::path Appearance::defaultFile(std::string_view slug) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / slug / "appearance.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / slug / "appearance.json";
    return {};
}

Appearance Appearance::load(const std::filesystem::path& file) {
    Appearance a;
    std::error_code ec;
    if (file.empty() || !std::filesystem::exists(file, ec)) return a;
    auto bytes = readFile(file);
    if (!bytes) return a;
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc || !doc->isObject()) return a;
    if (const json::Value* mode = doc->find("mode"); mode && mode->isString())
        a.dark = mode->asString() != "light";
    if (const json::Value* p = doc->find("palette"); p && p->isString())
        if (auto palette = ui::paletteFrom(p->asString())) a.palette = *palette;
    return a;
}

Status Appearance::save(const std::filesystem::path& file) const {
    if (file.empty()) return Error("appearance.file", "no config directory (HOME is not set)");
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.appearance/1.0";
    doc["mode"] = dark ? "dark" : "light";
    doc["palette"] = std::string(ui::paletteName(palette));
    return writeFileAtomic(file, json::writeCanonical(doc) + "\n");
}

}  // namespace supple::gui
