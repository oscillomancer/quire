// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Default drawing for every Ui control, plus the panel furniture and text
// helpers products build their own drawing from. All colour comes from the
// Theme. A product that wants a different knob overrides
// EditorBase::drawControl for that variant and calls these for the rest.
//
// Every function leaves NanoVG's state as it found it.
#pragma once

#include <functional>
#include <string>

#include <nanovg.h>

#include "gui/AudioHistory.h"
#include "gui/Meter.h"
#include "gui/Plot.h"
#include "gui/Spectrum.h"
#include "gui/Theme.h"
#include "gui/Ui.h"

namespace supple::gui::widgets {

enum class Align { Left, Center, Right };

// --- text ----------------------------------------------------------------------

// Words, in Inter.
void label(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour,
           Align align = Align::Left, bool bold = false);
// Numbers, in Plex Mono, with a typographic minus sign.
void number(NVGcontext* vg, float x, float y, const char* text, float size, NVGcolor colour,
            Align align = Align::Left, bool medium = true);
// Small spaced capitals for section and panel titles.
void caption(NVGcontext* vg, float x, float y, const char* text, NVGcolor colour,
             Align align = Align::Left, float size = 10.0f);
float textWidth(NVGcontext* vg, const char* text, float size, const char* face);
// The rule every control's text follows in its space: it shrinks from
// `size` toward `minSize`, and what still does not fit is cut at a
// character and ends in an ellipsis ("Low Cut Freq…"). Returns the size
// to draw at; `text` is cut to match. Draw the product's own text to it.
float fitText(NVGcontext* vg, std::string& text, float size, float minSize, float width, const char* face);
// How wide Ui's text field draws `text`: what it places the caret by.
float textEntryWidth(NVGcontext* vg, std::string_view text, bool ascii);

// --- furniture -------------------------------------------------------------------

void hairline(NVGcontext* vg, float x0, float y0, float x1, float y1, NVGcolor colour, float width = 1.0f);
// A raised panel with a soft shadow, optional title and rule.
void panel(NVGcontext* vg, const Theme& t, Rect r, const char* title = nullptr);
// A recessed well: the floor of a graph or meter.
void well(NVGcontext* vg, const Theme& t, Rect r, float radius = 4.0f);

// --- controls --------------------------------------------------------------------

// Draws any control in its default look.
void control(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void knob(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void hslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void vslider(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void toggle(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void segmented(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void dropdown(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// Label, value and a bar from the origin, for display only.
void readout(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// A property's text in a well; the control's label shows when it is empty.
void textField(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
void fileSlot(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// Previous, preset name (with * when modified), next, and A/B.
void presetBar(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// A raised face with its label; a momentary control is lit while held.
void button(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// A well with a point at (across, up), each parameter's name and value.
void xyPad(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// Labels in a row, the page showing underlined.
void tabs(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// Rows in a well, the chosen one lit, headers as captions, a scroll bar
// when they do not all fit.
void list(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// White keys, black keys, the held ones lit; each C named.
void keyboard(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);
// The envelope's shape with a handle per stage; the handle in use names
// its value.
void envelope(NVGcontext* vg, const Theme& t, const Control& c, const ControlState& s);

// The open menu, text field and tooltip, drawn above everything else.
void overlays(NVGcontext* vg, const Theme& t, const Ui& ui);
// Files are being dragged over a place that takes them.
void dropTarget(NVGcontext* vg, const Theme& t, Rect r);

// --- graphs ------------------------------------------------------------------------
// Each draws inside r with Axis (gui/Plot.h) for the scale; labels go just
// outside r, below it or to its left.

// Lines at 1, 2 and 5 of each decade, the decades brighter; "100", "1k".
void frequencyGrid(NVGcontext* vg, const Theme& t, Rect r, const Axis& hz, bool labels = true);
// Lines every `step` dB, 0 dB brightest.
void levelGrid(NVGcontext* vg, const Theme& t, Rect r, const Axis& db, double step, bool labels = true);
// y = f(x), one point per two design units: a response, a transfer curve.
// With fillTo, the area between the curve and that y value is shaded.
void plot(NVGcontext* vg, Rect r, const Axis& x, const Axis& y, const std::function<double(double)>& f,
          NVGcolor colour, float width = 2.0f, const double* fillTo = nullptr);
// A ValueHistory across r, the newest at the right.
void trace(NVGcontext* vg, Rect r, const ValueHistory& values, const Axis& y, NVGcolor colour, float width = 1.5f);
// A Spectrum: each column the loudest bin it covers, filled beneath.
void spectrum(NVGcontext* vg, Rect r, const Spectrum& s, const Axis& hz, const Axis& db, NVGcolor colour);
// The last `seconds` of a signal scrolling right to left, as its envelope
// (each column's lowest and highest sample), -1..+1 across r's height.
void waveform(NVGcontext* vg, Rect r, const AudioHistory& audio, Signal signal, double seconds, double sampleRate,
              NVGcolor colour);
// `seconds` of a signal from its latest rising zero crossing, so a steady
// tone stands still: an oscilloscope.
void oscilloscope(NVGcontext* vg, Rect r, const AudioHistory& audio, Signal signal, double seconds,
                  double sampleRate, NVGcolor colour, float width = 1.5f);

// --- meters ------------------------------------------------------------------------

// A vertical peak meter from PeakMeter::kFloorDb to +6 dBFS, one bar per
// channel, with hold marks and a clip lamp. With RMS levels (a product with
// kAudioFeed: EditorBase::outputRms()), each bar is the RMS, solid, with the
// peak above it fainter.
void meter(NVGcontext* vg, const Theme& t, Rect r, const PeakMeter* channels, int count,
           const RmsMeter* rms = nullptr);
// Gain reduction, a bar growing down from the top: 0 dB is none, `range`
// fills it. db is the reduction as a negative (or positive) number.
void reductionMeter(NVGcontext* vg, const Theme& t, Rect r, float db, float range = 24.0f);
// dBFS to a 0..1 meter position, matching meter().
float meterPosition(float db);

}  // namespace supple::gui::widgets
