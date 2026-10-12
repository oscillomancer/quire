// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What a product's editor derives from.
//
// A product editor builds its controls in its constructor (ui().knob(...)),
// draws the panel in paint(), and may react to telemetry. Everything else --
// input routing, gestures, keyboard focus, menus, text entry, repaint
// decisions, meter ballistics, fonts -- is here and in Ui.
//
// Repaint is decided in idle(), which Window calls on every timer tick
// whether or not anything was drawn: telemetry is drained there, and a
// parameter the host changed is noticed by comparing against what was last
// drawn. An idle editor (no audio, nobody touching it) draws nothing. An
// output parameter moves with the audio, so it repaints only when a visible
// control shows it or the product watch()es it.
//
// All coordinates are design units: the panel is laid out once at its
// design size and Window scales the whole drawing.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nanovg.h>

#include "core/Telemetry.h"
#include "gui/AudioHistory.h"
#include "gui/EditorHost.h"
#include "gui/Images.h"
#include "gui/Meter.h"
#include "gui/Spectrum.h"
#include "gui/Theme.h"
#include "gui/Ui.h"

namespace supple::gui {

class EditorBase {
  public:
    EditorBase(EditorHost& host, const Theme& theme, float width, float height);
    virtual ~EditorBase() = default;
    EditorBase(const EditorBase&) = delete;
    EditorBase& operator=(const EditorBase&) = delete;

    float width() const { return width_; }
    float height() const { return height_; }

    // --- for the product ------------------------------------------------------

    // The whole panel. Call drawControls() where the controls belong; menus
    // and text fields are drawn above everything afterwards.
    virtual void paint(NVGcontext* vg) = 0;
    // Each telemetry frame, in order. Call markDirty() if what it changes is
    // on screen; frames arrive during silence too.
    virtual void onTelemetry(const TelemetryFrame& frame) { (void)frame; }
    // Every idle tick, with real elapsed seconds. Advance animation here and
    // call markDirty() when something on screen moved.
    virtual void animate(double seconds) { (void)seconds; }
    // Custom interactive drawing: return true to take the event before Ui.
    // A press taken here holds the pointer until that button is released:
    // every move and the release come to these hooks, and the wheel and
    // keys do nothing meanwhile, so nothing else closes the gesture a drag
    // opened with Ui::dragEdit. A menu or text field Ui has open still
    // comes first.
    virtual bool onPointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
        (void)x, (void)y, (void)button, (void)mods, (void)doubleClick;
        return false;
    }
    virtual bool onPointerMove(float x, float y, uint32_t mods) {
        (void)x, (void)y, (void)mods;
        return false;
    }
    virtual bool onPointerUp(float x, float y, int button, uint32_t mods) {
        (void)x, (void)y, (void)button, (void)mods;
        return false;
    }
    // Focus was lost or the view is going away: drop any interaction the
    // pointer hooks started, as Ui drops its own. The release may never come.
    virtual void onCancel() {}
    // New audio arrived (a product that declared kAudioFeed): frames since
    // the last call. The default repaints, until three seconds of silence
    // have scrolled through; override to repaint only what moved.
    virtual void onAudio(size_t frames) {
        (void)frames;
        markDirty();
    }
    // Files dragged over the editor: true where they would be taken, and
    // onDrop() then gets them, as paths (UTF-8). Preset files dropped on an
    // editor with presets load without either, and so does a file dropped
    // on a file slot (Ui::fileSlot).
    virtual bool acceptsDrop(float x, float y) {
        (void)x, (void)y;
        return false;
    }
    virtual void onDrop(float x, float y, const std::vector<std::string>& paths) { (void)x, (void)y, (void)paths; }

    // --- for Window -----------------------------------------------------------

    bool attach(NVGcontext* vg);  // registers fonts; false if the context is unusable
    void detach();                // the context is going away
    void idle(double seconds);
    bool needsRepaint();
    void render(NVGcontext* vg);

    bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick);
    void pointerMove(float x, float y, uint32_t mods);
    void pointerUp(float x, float y, int button, uint32_t mods);
    void pointerLeave();
    bool wheel(float x, float y, float dx, float dy, uint32_t mods);
    bool key(uint32_t code, uint32_t mods);
    bool text(uint32_t codepoint);
    void focusLost();
    // A drag of files over (x, y): whether it would be taken here. Then
    // the drop itself.
    bool dropOver(float x, float y);
    void drop(float x, float y, const std::vector<std::string>& paths);
    // The pointer shape. Override to give custom interactive drawing its own.
    virtual Cursor cursor() const { return ui_.cursor(); }
    uint32_t background() const { return theme_.ground; }
    // The controls, read-only: for tests and tools that inspect the layout.
    const Ui& layout() const { return ui_; }
    void markDirty() { dirty_ = true; }
    // The window's clipboard, for copying and pasting settings; null when
    // there is none.
    void setClipboard(Clipboard* clipboard) { ui_.setClipboard(clipboard); }
    // The window's file dialogs; null when there are none.
    void setFileDialogs(FileDialogs* files) { ui_.setFileDialogs(files); }
    // Another palette, for the whole editor, from the next frame: a light
    // and a dark one, say. The product keeps both.
    void setTheme(const Theme& theme) {
        theme_ = theme;
        dirty_ = true;
    }

  protected:
    EditorHost& host() const { return host_; }
    Ui& ui() { return ui_; }
    const Ui& ui() const { return ui_; }
    const Theme& theme() const { return theme_; }

    // The track the plugin is on (name, colour), as the host last said;
    // the editor repaints when it changes.
    TrackInfo track() const { return host_.track(); }

    // The system's open and save dialogs (null in tests and tools).
    FileDialogs* fileDialogs() const { return ui_.fileDialogs(); }
    // Files are being dragged over the editor, and would be taken where
    // they are: draw the target lit. Where, in design units.
    bool dropping() const { return dropping_; }
    float dropX() const { return dropX_; }
    float dropY() const { return dropY_; }

    // The audio, for a product that declared kAudioFeed in its Processor:
    // the last 2.7 seconds (at 48 kHz) of the main input and output.
    const AudioHistory& audio() const { return *audio_; }
    // Their RMS levels, per channel (kAudioFeed): -3 dB for a full-scale sine.
    // When the host stops they fall as silence would, repainting as they do.
    const RmsMeter* inputRms() const { return inputRms_; }
    const RmsMeter* outputRms() const { return outputRms_; }

    // Pictures and icons from the product's assets (PRODUCT_ASSETS): an SVG
    // is drawn as vectors, crisp at any scale, optionally in one colour; a
    // PNG or JPEG is an image. Both are loaded once and kept. False when
    // there is no such asset or it would not decode.
    bool drawSvg(NVGcontext* vg, const char* asset, Rect r, const NVGcolor* tint = nullptr);
    bool drawImage(NVGcontext* vg, const char* asset, Rect r, float alpha = 1.0f);

    // A parameter's plain value, for drawing. An output's is what the
    // processor last reported.
    double plain(uint32_t id) const;
    // Repaint when this output parameter changes, for drawing of the
    // product's own that reads it. A control bound to it needs no call.
    void watch(uint32_t id);
    // The plain value the DSP is using: the base value plus any host
    // modulation. Draw what is heard (a response curve) from this.
    double effective(uint32_t id) const;

    void drawControls(NVGcontext* vg);
    // One control. Override to restyle some controls (by variant, say) and
    // fall back to this for the rest.
    virtual void drawControl(NVGcontext* vg, const Control& c, const ControlState& s);

    // A product that does not draw inputMeters() and outputMeters() turns
    // them off, or meters nobody sees keep the editor repainting.
    void usePeakMeters(bool on) { peakMeters_ = on; }
    const PeakMeter* inputMeters() const { return input_; }
    const PeakMeter* outputMeters() const { return output_; }
    int meterChannels() const { return meterChannels_; }

  private:
    EditorHost& host_;
    Theme theme_;
    float width_, height_;
    Ui ui_;
    std::vector<double> shownValue_;
    std::vector<double> shownModulation_;
    std::vector<bool> watched_;
    uint64_t shownProperties_ = 0;
    uint64_t shownTrack_ = 0;
    PeakMeter input_[kMeterChannels];
    PeakMeter output_[kMeterChannels];
    int meterChannels_ = kMeterChannels;
    bool peakMeters_ = true;
    bool dirty_ = true;
    bool attached_ = false;
    std::unique_ptr<AudioHistory> audio_;
    std::vector<FeedFrame> feedScratch_;
    RmsMeter inputRms_[2], outputRms_[2];
    double silentFrames_ = 0.0;
    double starved_ = 0.0;  // seconds since any audio arrived
    int held_ = -1;  // the button of a press onPointerDown() took, until it is released
    bool dropping_ = false;
    float dropX_ = 0.0f, dropY_ = 0.0f;
    double dropAge_ = 0.0;
    Images images_;
};

}  // namespace supple::gui

namespace product {
// Defined by the product, in product/Editor.cpp.
std::unique_ptr<supple::gui::EditorBase> createEditor(supple::gui::EditorHost& host);
}  // namespace product
