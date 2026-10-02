// The look the user picked (gui.md §7): dark or light, and the warm (orange,
// the default) or ink (blue) palette. Kept in <config>/<slug>/appearance.json
// (supple.appearance/1.0); View > switch actions change and save it.
#pragma once

#include <filesystem>
#include <string_view>

#include "supple/base/result.h"
#include "supple/ui/theme.h"

namespace supple::gui {

struct Appearance {
    bool dark = true;
    ui::Palette palette = ui::Palette::Warm;

    ui::Theme theme() const { return dark ? ui::Theme::dark(palette) : ui::Theme::light(palette); }
    static std::filesystem::path defaultFile(std::string_view slug);
    // The saved choice; the defaults when the file is missing or unreadable.
    static Appearance load(const std::filesystem::path& file);
    Status save(const std::filesystem::path& file) const;
};

}  // namespace supple::gui
