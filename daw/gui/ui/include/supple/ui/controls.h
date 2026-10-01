// Mixer controls shared by the track control panel and the mixer (gui.md
// §4.2, §4.4): a dB fader, a pan control, a stereo meter with peak hold, and
// a toggle (mute, solo, arm, monitor). Drags report a gesture: Begin, Move…,
// End — callers commit each move with the same cmd::Origin::gesture so a drag
// is one undo step. Values set from the model (setValue) never call back.
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "supple/ui/node.h"

namespace supple::ui {

enum class Gesture { Begin, Move, End, Set };  // Set: one-shot (double-click reset, wheel, keys)

// dB <-> 0..1 position: a square-root taper over -60..+12 dB, -inf below.
// Unity sits near 70 % of the travel; the bottom 2 % is silence.
double faderPosition(double db);
double faderDb(double position);
constexpr double kFaderSilenceDb = -150.0;

class Fader : public Node {
  public:
    enum class Orientation { Vertical, Horizontal };
    Fader(std::string id, Orientation orientation = Orientation::Vertical);
    std::function<void(double db, Gesture)> onChange;
    void setValue(double db);  // from the model; no callback during a drag
    double value() const { return db_; }
    bool dragging() const { return dragging_; }

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;

  private:
    Rect track() const;
    double positionAt(Point p) const;
    void change(double db, Gesture g);
    Orientation orientation_;
    double db_ = 0, pressDb_ = 0;
    Point press_{};
    bool dragging_ = false;
};

// A normalized slider: [0, 1] along its width, for values whose curve the
// caller owns (a plugin parameter through core-params, a wet/dry percent).
// Like the fader, a press never jumps the value: drags are relative to the
// press (Shift for fine), a double click resets to the caller's default and
// the keys/wheel step. Drags report a gesture, so a drag is one undo step.
class Slider : public Node {
  public:
    explicit Slider(std::string id);
    std::function<void(double value, Gesture)> onChange;
    void setValue(double value);  // from the model; no callback
    double value() const { return value_; }
    // What a double click (and the caller's "reset") returns to.
    void setDefault(double value) { defaultValue_ = std::clamp(value, 0.0, 1.0); }
    double defaultValue() const { return defaultValue_; }
    bool dragging() const { return dragging_; }
    static constexpr float kInset = 10;  // knob travel inset per side

    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;

  private:
    Rect track() const;
    void change(double value, Gesture g);
    double value_ = 0, pressValue_ = 0, defaultValue_ = 0;
    Point press_{};
    bool dragging_ = false;
};

// Pan -100 (left) .. +100 (right) as a horizontal bar from the centre.
class PanControl : public Node {
  public:
    explicit PanControl(std::string id);
    std::function<void(double pan, Gesture)> onChange;
    void setValue(double pan);
    double value() const { return pan_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override;

  private:
    void change(double pan, Gesture g);
    double pan_ = 0, pressPan_ = 0;
    Point press_{};
    bool dragging_ = false;
};

// Stereo peak/RMS meter, -60..+6 dBFS. Readings come from EngineApp::meters
// (what was heard since the last call); the meter decays and holds peaks
// itself and asks for animation frames while anything moves. A click resets
// the clip indicators.
class Meter : public Node {
  public:
    explicit Meter(std::string id, bool vertical = true);
    void push(const float peak[2], const float rms[2]);  // linear amplitudes
    void reset();
    bool clipped() const { return clip_[0] || clip_[1]; }
    float shownPeakDb(int channel) const { return peakDb_[channel]; }
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    void tick();  // decay (call every frame the meter is visible)

  private:
    bool vertical_;
    float peakDb_[2] = {-150, -150}, rmsDb_[2] = {-150, -150}, holdDb_[2] = {-150, -150};
    std::chrono::steady_clock::time_point holdAt_[2]{}, last_{};
    bool clip_[2] = {false, false};
};

// A latching button (mute, solo, arm): text, an "on" colour token.
class Toggle : public Node {
  public:
    Toggle(std::string id, std::string text, std::string onColor,
           std::function<void(bool)> onToggle = {});
    std::function<void(bool)> onToggle;
    void setOn(bool on);
    bool on() const { return on_; }
    Size measure(const Canvas& canvas, Size available) override;
    void paint(Canvas& canvas) override;
    bool onPointer(const PointerEvent& e) override;
    bool onKey(const KeyEvent& e) override;
    std::string valueText() const override { return on_ ? "on" : "off"; }

  private:
    std::string text_, onColor_;
    bool on_ = false, pressed_ = false;
};

}  // namespace supple::ui
