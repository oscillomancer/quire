// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/EditorBase.h"

#include <algorithm>
#include <cmath>

#include "core/Assets.h"
#include "core/PresetLibrary.h"
#include "gui/Fonts.h"
#include "gui/Widgets.h"

namespace supple::gui {

EditorBase::EditorBase(EditorHost& host, const Theme& theme, float width, float height)
    : host_(host),
      theme_(theme),
      width_(width),
      height_(height),
      ui_(host, width, height),
      shownValue_(host.pluginDefinition().params().count(), 0.0),
      shownModulation_(host.pluginDefinition().params().count(), 0.0),
      watched_(host.pluginDefinition().params().count(), false) {
    if constexpr (product::Processor::kAudioFeed) {
        audio_ = std::make_unique<AudioHistory>();
        feedScratch_.resize(2048);
    }
}

void EditorBase::watch(uint32_t id) {
    const int i = paramTable().indexOf(id);
    if (i >= 0) watched_[static_cast<size_t>(i)] = true;
}

bool EditorBase::attach(NVGcontext* vg) {
    attached_ = loadFonts(vg);
    if (attached_)
        ui_.setTextWidth([vg](std::string_view text, bool ascii) { return widgets::textEntryWidth(vg, text, ascii); });
    dirty_ = true;
    return attached_;
}

void EditorBase::detach() {
    focusLost();
    ui_.setTextWidth(nullptr);
    // The context frees its images itself as it goes.
    images_.detach(nullptr);
    attached_ = false;
}

double EditorBase::plain(uint32_t id) const {
    const ParamDef* p = paramTable().find(id);
    return p ? toPlain(*p, host_.value(id)) : 0.0;
}

double EditorBase::effective(uint32_t id) const {
    const ParamDef* p = paramTable().find(id);
    return p ? toPlain(*p, sanitizeHost(*p, host_.value(id) + host_.modulation(id))) : 0.0;
}

void EditorBase::idle(double seconds) {
    // Frames alone do not force a repaint: silence produces frames too. The
    // meters say when they moved, and a product's onTelemetry() marks dirty
    // when what it shows changed.
    TelemetryFrame frame;
    while (host_.popTelemetry(frame)) {
        meterChannels_ = frame.channels > 0 ? frame.channels : meterChannels_;
        for (int c = 0; c < kMeterChannels; ++c) {
            input_[c].push(frame.inputPeak[c]);
            output_[c].push(frame.outputPeak[c]);
        }
        ui_.setKeysHeld(frame.keys);
        onTelemetry(frame);
    }
    if (audio_) {
        // Everything that arrived since the last tick, then the product's say.
        size_t arrived = 0;
        const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
        for (size_t n; (n = host_.readAudio(feedScratch_.data(), feedScratch_.size())) > 0;) {
            audio_->push(feedScratch_.data(), n);
            arrived += n;
            float loudest = 0.0f;
            float left[2048], right[2048];
            for (size_t k = 0; k < n; ++k) {
                const FeedFrame& f = feedScratch_[k];
                left[k] = f.in[0];
                right[k] = f.in[1];
                loudest = std::max({loudest, std::fabs(f.in[0]), std::fabs(f.in[1]), std::fabs(f.out[0]), std::fabs(f.out[1])});
            }
            inputRms_[0].push(left, n, rate);
            inputRms_[1].push(right, n, rate);
            for (size_t k = 0; k < n; ++k) {
                left[k] = feedScratch_[k].out[0];
                right[k] = feedScratch_[k].out[1];
            }
            outputRms_[0].push(left, n, rate);
            outputRms_[1].push(right, n, rate);
            silentFrames_ = loudest > 1e-6f ? 0.0 : silentFrames_ + static_cast<double>(n);
        }
        // Three seconds of silence scroll through and settle; then nothing moves.
        if (arrived > 0 && silentFrames_ < 3.0 * rate) onAudio(arrived);
        // The host stopped (longer than any block takes): the RMS meters fall
        // as silence would make them.
        if (arrived > 0) {
            starved_ = 0.0;
        } else if ((starved_ += seconds) > 0.25) {
            for (RmsMeter* m : {&inputRms_[0], &inputRms_[1], &outputRms_[0], &outputRms_[1]}) {
                if (m->moving()) dirty_ = true;
                m->fall(seconds);
            }
        }
    }
    if (dropping_ && (dropAge_ += seconds) > 0.3) {  // the drag left, or was let go elsewhere
        dropping_ = false;
        dirty_ = true;
    }
    bool moving = false;
    for (int c = 0; c < kMeterChannels; ++c) {
        moving = moving || input_[c].moving() || output_[c].moving();
        input_[c].advance(seconds);
        output_[c].advance(seconds);
        moving = moving || input_[c].moving() || output_[c].moving();
    }
    if (moving && peakMeters_) dirty_ = true;
    ui_.idle(seconds);
    animate(seconds);
}

bool EditorBase::needsRepaint() {
    if (ui_.takeDirty()) dirty_ = true;
    if (dirty_) return true;
    if (host_.propertiesVersion() != shownProperties_) return dirty_ = true;
    if (host_.trackVersion() != shownTrack_) return dirty_ = true;
    const ParamTable& params = paramTable();
    for (size_t i = 0; i < params.count(); ++i) {
        const uint32_t id = params.at(i).id;
        if (params.at(i).isOutput() && !watched_[i]) {
            bool shown = false;
            for (int c = 0; c < ui_.count() && !shown; ++c) {
                const Control& k = ui_.control(c);
                if (k.widget == Widget::TextField || !ui_.shown(c)) continue;
                for (int slot = 0; slot < Control::kSlots; ++slot) shown = shown || k.paramAt(slot) == id;
            }
            if (!shown) continue;  // nobody sees it move
        }
        if (host_.value(id) != shownValue_[i]) return dirty_ = true;
        if (params.at(i).is(pflag::kModulatable) && host_.modulation(id) != shownModulation_[i])
            return dirty_ = true;
    }
    return false;
}

void EditorBase::render(NVGcontext* vg) {
    if (!attached_) return;
    // Recorded before drawing, so a change that lands mid-frame repaints again.
    shownProperties_ = host_.propertiesVersion();
    shownTrack_ = host_.trackVersion();
    const ParamTable& params = paramTable();
    for (size_t i = 0; i < params.count(); ++i) {
        const uint32_t id = params.at(i).id;
        shownValue_[i] = host_.value(id);
        shownModulation_[i] = host_.modulation(id);
    }
    ui_.applyRules();
    paint(vg);
    if (dropping_) {
        // Over a file slot, the slot is the target; elsewhere, the editor.
        const int slot = ui_.fileSlotAt(dropX_, dropY_, {});
        widgets::dropTarget(vg, theme_, slot >= 0 ? ui_.control(slot).bounds : Rect{0.0f, 0.0f, width_, height_});
    }
    widgets::overlays(vg, theme_, ui_);
    ui_.takeDirty();  // drawn: whatever Ui changed is on screen now
    dirty_ = false;
}

void EditorBase::drawControls(NVGcontext* vg) {
    for (int i = 0; i < ui_.count(); ++i) {
        const Control& c = ui_.control(i);
        if (ui_.shown(i)) drawControl(vg, c, ui_.state(i));
    }
}

void EditorBase::drawControl(NVGcontext* vg, const Control& c, const ControlState& s) {
    widgets::control(vg, theme_, c, s);
}

// --- input -------------------------------------------------------------------------

bool EditorBase::pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
    // Overlays belong to Ui and sit above any custom drawing.
    const bool overlay = ui_.menu().open || ui_.textEdit().open;
    if (!overlay && (onPointerDown(x, y, button, mods, doubleClick) || held_ >= 0)) {
        if (held_ < 0) held_ = button;
        dirty_ = true;
        return true;
    }
    return ui_.pointerDown(x, y, button, mods, doubleClick);
}

void EditorBase::pointerMove(float x, float y, uint32_t mods) {
    const bool overlay = ui_.menu().open || ui_.textEdit().open;
    if (!overlay && held_ >= 0) {
        onPointerMove(x, y, mods);
        dirty_ = true;
        return;
    }
    if (!overlay && !ui_.dragging() && onPointerMove(x, y, mods)) {
        dirty_ = true;
        return;
    }
    ui_.pointerMove(x, y, mods);
}

void EditorBase::pointerUp(float x, float y, int button, uint32_t mods) {
    if (held_ < 0) {
        ui_.pointerUp(x, y, button, mods);
        return;
    }
    if (button == held_) held_ = -1;
    onPointerUp(x, y, button, mods);
    dirty_ = true;
}

void EditorBase::pointerLeave() { ui_.pointerLeave(); }

bool EditorBase::wheel(float x, float y, float dx, float dy, uint32_t mods) {
    if (held_ >= 0) return true;  // a custom drag is under way
    return ui_.wheel(x, y, dx, dy, mods);
}

bool EditorBase::key(uint32_t code, uint32_t mods) {
    if (held_ >= 0 && !ui_.menu().open && !ui_.textEdit().open) return true;
    return ui_.key(code, mods);
}

bool EditorBase::text(uint32_t codepoint) { return ui_.text(codepoint); }

void EditorBase::focusLost() {
    held_ = -1;
    onCancel();
    ui_.focusLost();
    dirty_ = true;
}

// --- files dropped -------------------------------------------------------------------

bool EditorBase::dropOver(float x, float y) {
    const bool wanted = ui_.fileSlotAt(x, y, {}) >= 0 || acceptsDrop(x, y) || host_.presetLibrary() != nullptr;
    if (wanted != dropping_ || x != dropX_ || y != dropY_) dirty_ = true;
    dropping_ = wanted;
    dropX_ = x;
    dropY_ = y;
    dropAge_ = 0.0;
    return wanted;
}

void EditorBase::drop(float x, float y, const std::vector<std::string>& paths) {
    dropping_ = false;
    dirty_ = true;
    if (paths.empty()) return;
    // Preset files load as presets, wherever they land.
    bool presets = true;
    for (const std::string& p : paths) presets = presets && assets::hasExtension(p, PresetLibrary::kExtension);
    if (presets) {
        if (PresetLibrary* library = host_.presetLibrary()) library->loadFile(PresetLibrary::fromUtf8(paths.front()));
        return;
    }
    if (ui_.dropFiles(x, y, paths)) return;
    if (acceptsDrop(x, y)) onDrop(x, y, paths);
}

// --- pictures --------------------------------------------------------------------------

bool EditorBase::drawSvg(NVGcontext* vg, const char* asset, Rect r, const NVGcolor* tint) {
    const Svg* svg = images_.svg(asset);
    if (!svg) return false;
    svg->draw(vg, r, tint);
    return true;
}

bool EditorBase::drawImage(NVGcontext* vg, const char* asset, Rect r, float alpha) {
    const int image = images_.image(vg, asset);
    if (image <= 0) return false;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, r.x, r.y, r.w, r.h);
    nvgFillPaint(vg, nvgImagePattern(vg, r.x, r.y, r.w, r.h, 0.0f, image, alpha));
    nvgFill(vg);
    nvgRestore(vg);
    return true;
}

}  // namespace supple::gui
