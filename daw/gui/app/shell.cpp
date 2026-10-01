#include "shell.h"

#include <nanovg.h>

#include <cctype>
#include <functional>

#include "supple/model/objects.h"
#include "supple/product.h"

namespace supple::gui {
namespace {
// Placeholder palette from the identity proposal (gui.md §7: warm graphite,
// ink, strip types as bindings) until the theme tokens of P5.2 exist.
NVGcolor rgb(std::uint32_t c) {
    return nvgRGB(static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8),
                  static_cast<unsigned char>(c));
}
constexpr std::uint32_t kPanel = 0x24211d, kRule = 0x3a352f, kInk = 0xebe6da, kQuiet = 0x9d968b,
                        kChannel = 0x7fa3cf, kBuss = 0xc9a14a, kMaster = 0xc0634f;

std::uint32_t typeColor(model::StripType t) {
    switch (t) {
        case model::StripType::Channel:
            return kChannel;
        case model::StripType::Buss:
            return kBuss;
        case model::StripType::Master:
            return kMaster;
    }
    return kInk;
}

void text(NVGcontext* vg, int font, float size, std::uint32_t color, float x, float y,
          const std::string& s, int align = NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE) {
    nvgFontFaceId(vg, font);
    nvgFontSize(vg, size);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, rgb(color));
    nvgText(vg, x, y, s.c_str(), nullptr);
}
}  // namespace

void Shell::draw(NVGcontext* vg, float width, float height) {
    const model::Session& s = app_.head();
    drawnVersion_ = s.version;

    // Transport line.
    constexpr float bar = 44.0f;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, width, bar);
    nvgFillColor(vg, rgb(kPanel));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, 0, bar - 1, width, 1);
    nvgFillColor(vg, rgb(kRule));
    nvgFill(vg);
    text(vg, fonts_.uiBold, 15, kInk, 16, bar / 2, std::string(product::kName));
    text(vg, fonts_.monoBold, 15, kInk, 120, bar / 2, "1.1.000");
    text(vg, fonts_.mono, 13, kQuiet, width - 16, bar / 2,
         std::to_string(s.tracks.size()) + " strips  ·  version " + std::to_string(s.version),
         NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);

    // Strip tree: masters first, children indented under their parent.
    float y = bar + 8;
    constexpr float row = 30.0f;
    std::function<void(model::Id, int)> strip = [&](model::Id id, int depth) {
        const model::Track* t = s.tracks.find(id);
        if (!t || y > height) return;
        const float x = 16.0f + 18.0f * static_cast<float>(depth);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y + 6, 22, row - 12, 3);
        nvgFillColor(vg, rgb(typeColor(t->type)));
        nvgFill(vg);
        const std::string letter(
            1, static_cast<char>(std::toupper(model::stripTypeName(t->type).front())));
        text(vg, fonts_.uiBold, 11, kPanel, x + 11, y + row / 2, letter,
             NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        text(vg, fonts_.ui, 14, kInk, x + 32, y + row / 2,
             t->name.empty() ? std::string(model::stripTypeName(t->type)) : t->name);
        y += row;
        for (model::Id child : t->children) strip(child, depth + 1);
    };
    for (model::Id master : s.project->masters) strip(master, 0);

    text(vg, fonts_.mono, 12, kQuiet, 16, height - 14, status_);
}

}  // namespace supple::gui
