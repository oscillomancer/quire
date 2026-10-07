// Tempo and meter maps and time conversion (spec: docs/specs/session-model.md
// §1.3). Pure and immutable; safe to share across threads once built.
//
// Positions live in two domains: seconds, and beats (quarter notes from the
// project start, beat 0 = the downbeat of bar 1). Samples appear only when a
// render plan is compiled (samplesAt).
//
// Tempo: points {beat, bpm, shape}. `Step` holds the tempo until the next
// point; `Ramp` changes BPM linearly *in beats* up to the next point (the
// last point always holds). Before beat 0 and after the last point the tempo
// holds. Meter: changes {bar, numerator, denominator} at bar starts; bar 1 is
// the first bar; bars before 1 (pickups) extend the first meter backwards.
#pragma once

#include <cstdint>
#include <vector>

#include "supple/base/result.h"

namespace supple::time {

enum class TempoShape : std::uint8_t { Step, Ramp };

struct TempoPoint {
    double beat = 0.0;
    double bpm = 120.0;
    TempoShape shape = TempoShape::Step;
    friend bool operator==(const TempoPoint&, const TempoPoint&) = default;
};

struct MeterChange {
    int bar = 1;
    int numerator = 4;
    int denominator = 4;
    friend bool operator==(const MeterChange&, const MeterChange&) = default;
};

// A musical position for display and editing: bar (1-based), beat (1-based,
// in units of the meter's denominator, as a DAW ruler shows it) and the
// fraction of that beat in [0, 1). `quarters` is the same position in
// quarter notes from the start of the bar.
struct BarBeat {
    int bar = 1;
    int beat = 1;
    double fraction = 0.0;
    double quarters = 0.0;
};

class TimeMap {
  public:
    // A constant tempo and meter.
    TimeMap();
    static TimeMap constant(double bpm, int numerator = 4, int denominator = 4);
    // Tempo points closer than this have no finite ramp slope to speak of.
    static constexpr double kMinTempoSpacing = 1e-6;  // beats
    // Validates: tempo points increasing by at least kMinTempoSpacing, the
    // first at beat 0, bpm in [1, 1000]; meter changes strictly increasing,
    // the first at bar 1, numerator 1..64, denominator a power of two 1..64.
    // Errors carry paths (/tempo/<i>/beat, /meter/<i>/bar, …) — code
    // "time.map".
    static Result<TimeMap> create(std::vector<TempoPoint> tempo, std::vector<MeterChange> meter);

    const std::vector<TempoPoint>& tempo() const { return tempo_; }
    const std::vector<MeterChange>& meter() const { return meter_; }

    // Beats <-> seconds. Monotonic; exact inverses up to rounding (within
    // 1e-9 beats), closed-form on ramps.
    double secondsAt(double beat) const;
    double beatAt(double seconds) const;
    double bpmAt(double beat) const;

    // Meter and bars.
    MeterChange meterAt(int bar) const;  // the change in effect (its `bar` is where it began)
    double beatOfBar(int bar) const;     // start of `bar` in beats (bar 1 -> 0)
    double barLength(int bar) const;     // in quarter notes
    // Clamps to bars [INT_MIN, INT_MAX], including the final bar's last
    // fraction. Infinities clamp; NaN selects beat 0.
    BarBeat barBeat(double beat) const;  // positions within 1e-9 of a bar line snap onto it
    double beatOf(const BarBeat& position) const;  // uses bar, beat, fraction (not quarters)

    // Maps are equal when their points and changes are (the rest is derived).
    friend bool operator==(const TimeMap& a, const TimeMap& b) {
        return a.tempo_ == b.tempo_ && a.meter_ == b.meter_;
    }

  private:
    TimeMap(std::vector<TempoPoint> tempo, std::vector<MeterChange> meter);  // validated input
    struct TempoSeg {
        double beat, seconds, bpm, slope;  // slope: BPM per beat (0 for steps)
    };
    struct MeterSeg {
        int bar;
        double beat, barQuarters;
        int numerator, denominator;
    };
    void build();
    std::size_t tempoSegAtBeat(double beat) const;
    std::size_t tempoSegAtSeconds(double seconds) const;
    std::size_t meterSegAtBar(int bar) const;
    std::size_t meterSegAtBeat(double beat) const;

    std::vector<TempoPoint> tempo_;
    std::vector<MeterChange> meter_;
    std::vector<TempoSeg> tempoSegs_;
    std::vector<MeterSeg> meterSegs_;
};

// Seconds -> samples at `rate`, rounding half to even (the spec's rule for
// compiling render plans). Saturates to [INT64_MIN, INT64_MAX], including
// infinite products; a NaN product returns 0.
std::int64_t samplesAt(double seconds, double rate);

}  // namespace supple::time
