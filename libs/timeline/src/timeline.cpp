#include "supple/time/timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace supple::time {

namespace {

constexpr double kSnap = 1e-9;  // quarter notes

// log1p(x)/x and expm1(x)/x, accurate near 0 (a ramp between nearly equal
// tempos must not lose precision to cancellation).
double log1pOverX(double x) {
    if (std::fabs(x) < 1e-8) return 1.0 - x / 2.0 + x * x / 3.0;
    return std::log1p(x) / x;
}
double expm1OverX(double x) {
    if (std::fabs(x) < 1e-8) return 1.0 + x / 2.0 + x * x / 6.0;
    return std::expm1(x) / x;
}

bool powerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

}  // namespace

TimeMap::TimeMap() : TimeMap(constant(120.0)) {}

TimeMap TimeMap::constant(double bpm, int numerator, int denominator) {
    auto m = create({{0.0, bpm, TempoShape::Step}}, {{1, numerator, denominator}});
    if (m) return std::move(*m);
    // Invalid arguments: fall back to 120 BPM 4/4 rather than an invalid map.
    TimeMap fallback = *create({{0.0, 120.0, TempoShape::Step}}, {{1, 4, 4}});
    return fallback;
}

Result<TimeMap> TimeMap::create(std::vector<TempoPoint> tempo, std::vector<MeterChange> meter) {
    Error all("time.map", "invalid tempo/meter map");
    auto bad = [&](std::string path, std::string message, std::string hint) {
        all.details.emplace_back("time.map", std::move(message), std::move(path), std::move(hint));
    };
    if (tempo.empty()) bad("/tempo", "the tempo map is empty", "add a point at beat 0");
    for (std::size_t i = 0; i < tempo.size(); ++i) {
        const std::string p = "/tempo/" + std::to_string(i);
        const TempoPoint& t = tempo[i];
        if (i == 0 && t.beat != 0.0)
            bad(p + "/beat", "the first tempo point must be at beat 0", "set beat 0");
        if (i > 0 && !(t.beat > tempo[i - 1].beat))
            bad(p + "/beat", "tempo points must be in increasing beat order",
                "sort them, no duplicates");
        if (!std::isfinite(t.beat)) bad(p + "/beat", "beat is not finite", "");
        if (!(t.bpm >= 1.0 && t.bpm <= 1000.0))
            bad(p + "/bpm", "tempo out of range", "1 .. 1000 BPM");
    }
    if (meter.empty()) bad("/meter", "the meter map is empty", "add a change at bar 1");
    for (std::size_t i = 0; i < meter.size(); ++i) {
        const std::string p = "/meter/" + std::to_string(i);
        const MeterChange& m = meter[i];
        if (i == 0 && m.bar != 1)
            bad(p + "/bar", "the first meter change must be at bar 1", "set bar 1");
        if (i > 0 && !(m.bar > meter[i - 1].bar))
            bad(p + "/bar", "meter changes must be in increasing bar order",
                "sort them, no duplicates");
        if (m.numerator < 1 || m.numerator > 64)
            bad(p + "/numerator", "numerator out of range", "1 .. 64");
        if (!powerOfTwo(m.denominator) || m.denominator > 64)
            bad(p + "/denominator", "denominator must be a power of two",
                "1, 2, 4, 8, 16, 32 or 64");
    }
    if (!all.details.empty()) {
        if (all.details.size() == 1) return all.details.front();
        return all;
    }
    TimeMap map(std::move(tempo), std::move(meter));
    return map;
}

TimeMap::TimeMap(std::vector<TempoPoint> tempo, std::vector<MeterChange> meter)
    : tempo_(std::move(tempo)), meter_(std::move(meter)) {
    build();
}

void TimeMap::build() {
    tempoSegs_.clear();
    meterSegs_.clear();
    double seconds = 0.0;
    for (std::size_t i = 0; i < tempo_.size(); ++i) {
        const TempoPoint& p = tempo_[i];
        double slope = 0.0;
        if (p.shape == TempoShape::Ramp && i + 1 < tempo_.size())
            slope = (tempo_[i + 1].bpm - p.bpm) / (tempo_[i + 1].beat - p.beat);
        tempoSegs_.push_back({p.beat, seconds, p.bpm, slope});
        if (i + 1 < tempo_.size()) {
            const double db = tempo_[i + 1].beat - p.beat;
            const double x = slope * db / p.bpm;
            seconds += 60.0 * db / p.bpm * (slope == 0.0 ? 1.0 : log1pOverX(x));
        }
    }
    double beat = 0.0;
    for (std::size_t i = 0; i < meter_.size(); ++i) {
        const MeterChange& m = meter_[i];
        if (i > 0) beat += (m.bar - meter_[i - 1].bar) * meterSegs_.back().barQuarters;
        meterSegs_.push_back(
            {m.bar, beat, 4.0 * m.numerator / m.denominator, m.numerator, m.denominator});
    }
}

std::size_t TimeMap::tempoSegAtBeat(double beat) const {
    auto it = std::upper_bound(tempoSegs_.begin(), tempoSegs_.end(), beat,
                               [](double b, const TempoSeg& s) { return b < s.beat; });
    return it == tempoSegs_.begin() ? 0 : static_cast<std::size_t>(it - tempoSegs_.begin()) - 1;
}

std::size_t TimeMap::tempoSegAtSeconds(double seconds) const {
    auto it = std::upper_bound(tempoSegs_.begin(), tempoSegs_.end(), seconds,
                               [](double t, const TempoSeg& s) { return t < s.seconds; });
    return it == tempoSegs_.begin() ? 0 : static_cast<std::size_t>(it - tempoSegs_.begin()) - 1;
}

std::size_t TimeMap::meterSegAtBar(int bar) const {
    auto it = std::upper_bound(meterSegs_.begin(), meterSegs_.end(), bar,
                               [](int b, const MeterSeg& s) { return b < s.bar; });
    return it == meterSegs_.begin() ? 0 : static_cast<std::size_t>(it - meterSegs_.begin()) - 1;
}

std::size_t TimeMap::meterSegAtBeat(double beat) const {
    auto it = std::upper_bound(meterSegs_.begin(), meterSegs_.end(), beat,
                               [](double b, const MeterSeg& s) { return b < s.beat; });
    return it == meterSegs_.begin() ? 0 : static_cast<std::size_t>(it - meterSegs_.begin()) - 1;
}

double TimeMap::secondsAt(double beat) const {
    if (beat < 0.0) return 60.0 * beat / tempoSegs_.front().bpm;  // the first tempo holds
    const TempoSeg& s = tempoSegs_[tempoSegAtBeat(beat)];
    const double db = beat - s.beat;
    if (s.slope == 0.0) return s.seconds + 60.0 * db / s.bpm;
    // t = t0 + 60/k · ln(v(b)/v0) with v linear in beats.
    return s.seconds + 60.0 * db / s.bpm * log1pOverX(s.slope * db / s.bpm);
}

double TimeMap::beatAt(double seconds) const {
    if (seconds < 0.0) return seconds * tempoSegs_.front().bpm / 60.0;
    const TempoSeg& s = tempoSegs_[tempoSegAtSeconds(seconds)];
    const double dt = seconds - s.seconds;
    if (s.slope == 0.0) return s.beat + dt * s.bpm / 60.0;
    // b = b0 + v0/k · (exp(k·dt/60) − 1)
    return s.beat + s.bpm * dt / 60.0 * expm1OverX(s.slope * dt / 60.0);
}

double TimeMap::bpmAt(double beat) const {
    if (beat < 0.0) return tempoSegs_.front().bpm;
    const TempoSeg& s = tempoSegs_[tempoSegAtBeat(beat)];
    return s.bpm + s.slope * (beat - s.beat);
}

MeterChange TimeMap::meterAt(int bar) const {
    const MeterSeg& s = meterSegs_[meterSegAtBar(bar)];
    return {s.bar, s.numerator, s.denominator};
}

double TimeMap::barLength(int bar) const { return meterSegs_[meterSegAtBar(bar)].barQuarters; }

double TimeMap::beatOfBar(int bar) const {
    const MeterSeg& s = meterSegs_[meterSegAtBar(bar)];
    return s.beat + (static_cast<double>(bar) - s.bar) *
                        s.barQuarters;  // bars before 1 extend the first meter
}

BarBeat TimeMap::barBeat(double beat) const {
    constexpr int lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
    if (std::isnan(beat)) beat = 0;
    if (beat <= beatOfBar(lo)) return {lo, 1, 0, 0};
    const auto last = meterAt(hi);
    const double lastLength = barLength(hi);
    if (beat >= beatOfBar(hi) + lastLength - kSnap)
        return {hi, last.numerator, std::nextafter(1.0, 0.0), std::nextafter(lastLength, 0.0)};
    const MeterSeg& s = meterSegs_[beat < 0.0 ? 0 : meterSegAtBeat(beat)];
    const double L = s.barQuarters;
    const double k = std::floor((beat - s.beat) / L);
    int bar =
        static_cast<int>(std::clamp(s.bar + k, static_cast<double>(lo), static_cast<double>(hi)));
    double inBar = beat - (s.beat + k * L);
    if (inBar > L - kSnap && bar < hi) {  // a hair before the next bar line is on it
        ++bar;
        inBar = 0.0;
        if (beat >= 0.0 && meterSegAtBar(bar) != meterSegAtBar(bar - 1)) {
            // The next bar starts a new meter: re-derive from it.
            return barBeat(beatOfBar(bar));
        }
    }
    if (inBar < kSnap) inBar = 0.0;
    const double unit = 4.0 / (beat < 0.0 ? meterSegs_.front().denominator : s.denominator);
    double beats = inBar / unit;
    double whole = std::floor(beats);
    double fraction = beats - whole;
    if (fraction > 1.0 - kSnap / unit) {
        whole += 1.0;
        fraction = 0.0;
    }
    return {bar, static_cast<int>(whole) + 1, fraction, inBar};
}

double TimeMap::beatOf(const BarBeat& p) const {
    const MeterSeg& s = meterSegs_[meterSegAtBar(p.bar)];
    return beatOfBar(p.bar) +
           ((static_cast<double>(p.beat) - 1) + p.fraction) * 4.0 / s.denominator;
}

std::int64_t samplesAt(double seconds, double rate) {
    const double samples = std::nearbyint(seconds * rate);  // default mode: half to even
    if (std::isnan(samples)) return 0;
    if (samples >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    if (samples <= static_cast<double>(std::numeric_limits<std::int64_t>::min()))
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(samples);
}

}  // namespace supple::time
