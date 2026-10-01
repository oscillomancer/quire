// Theme tokens (docs/specs/gui.md §2, §7): widgets read named tokens, never
// literals. Loaded from `supple.theme/1.0` JSON; unknown names fall back to a
// loud magenta so a missing token is visible, not silently wrong.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/json/value.h"

namespace supple::ui {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static Color hex(std::uint32_t rgb, float alpha = 1.0f) {
        return {static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                static_cast<float>(rgb & 0xff) / 255.0f, alpha};
    }
    Color withAlpha(float alpha) const { return {r, g, b, alpha}; }
    bool operator==(const Color&) const = default;
};

class Theme {
  public:
    // The default themes from the identity proposal (paper and ink).
    static Theme dark();
    static Theme light();
    // Overrides on top of `base` (every token optional).
    static Result<Theme> fromJson(const json::Value& document, const Theme& base);

    Color color(std::string_view token) const;
    float type(std::string_view token) const;    // font sizes
    float space(std::string_view token) const;   // gaps and padding
    float radius(std::string_view token) const;  // corner radii
    std::string_view name() const { return name_; }

  private:
    std::string name_;
    std::map<std::string, Color, std::less<>> colors_;
    std::map<std::string, float, std::less<>> type_, space_, radius_;
};

}  // namespace supple::ui
