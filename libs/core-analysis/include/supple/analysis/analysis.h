// Offline analysis reports (spec: docs/specs/core-analysis.md §2–§8):
// loudness, spectral, harmonic, envelope and stereo descriptors of a signal,
// their JSON report, prose summary, comparison, target profiles and masking.
// Deterministic; offline (allocates); internal math in double.
#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "supple/analysis/loudness.h"
#include "supple/base/result.h"
#include "supple/json/json.h"

namespace supple::analysis {

enum class Detail { Summary, Standard, Full };

struct Options {
    bool loudness = true, spectral = true, harmonic = true, envelope = true, stereo = true;
    std::optional<double> noteOffSeconds;
    std::optional<std::pair<double, double>> f0RangeHz;  // default 30..4000 Hz
    // Cost controls for repeated analysis (the optimizer): analyze at most
    // this many evenly spaced pitch frames (0 = every 10 ms hop), and skip the
    // two extra loudness passes of stereo mono-loss.
    int pitchFrames = 0;
    bool monoLoss = true;
};

// Summary statistics over non-silent frames (NaN when there are none).
struct Stats {
    double mean = 0.0, median = 0.0, p10 = 0.0, p90 = 0.0;
};

struct LoudnessReport {
    double integrated = 0.0, lra = 0.0, maxMomentary = 0.0, maxShortTerm = 0.0;
    double truePeak = 0.0, samplePeak = 0.0, plr = 0.0;  // LUFS, LU, LUFS, LUFS, dBTP, dBFS, LU
    std::vector<double> momentary, shortTerm;            // 10 Hz series (full detail)
};

inline constexpr int kBandCount = 7;      // sub low low_mid mid high_mid presence air
inline constexpr int kThirdOctaves = 31;  // ISO 1/3-octave bands 20 Hz..20 kHz
std::string_view bandName(int band);
double thirdOctaveCentre(int band);

struct Band {
    double lowHz = 0.0, highHz = 0.0;
    double levelDb = 0.0;  // relative to the total power in 20 Hz..20 kHz
    double sharePct = 0.0;
};

struct SpectralReport {
    Stats centroidHz, rolloffHz, flatness, flux;
    std::array<Band, kBandCount> bands{};
    std::array<double, kThirdOctaves> ltasDb{};  // dBFS-scaled (full-scale sine = 0 dB)
    double tiltDbPerOct = 0.0;
    std::vector<double> centroidTrack;  // ≤ 200 points (full detail)
};

struct HarmonicReport {
    bool pitched = false;
    double voicedPct = 0.0;
    // Only meaningful when pitched:
    double f0Hz = 0.0, stabilityCents = 0.0, inharmonicity = 0.0, hnrDb = 0.0;
    std::string note;                 // "A3+4c"
    std::vector<double> harmonicsDb;  // k = 1..16, relative to k = 1
    std::vector<double> f0Track;      // Hz per 10 ms hop, 0 = unvoiced (full detail)
};

struct EnvelopeReport {
    double attackMs = 0.0;
    std::optional<double> decay20Ms;
    std::optional<double> releaseMs;
    bool releaseExtrapolated = false;
    std::optional<double> sustainDb;
    double crestDb = 0.0;
    double onsetsPerSecond = 0.0;
    std::vector<double> onsetTimes;  // seconds (full detail)
};

struct StereoReport {
    double correlation = 0.0, correlationMean = 0.0, correlationP5 = 0.0;
    double width = 0.0;
    std::array<double, kBandCount> bandWidth{};
    double balanceDb = 0.0;
    double monoLossLu = 0.0;
};

struct Report {
    double seconds = 0.0, sampleRate = 0.0;
    int channels = 0;
    Layout layout = Layout::Stereo;
    std::optional<LoudnessReport> loudness;
    std::optional<SpectralReport> spectral;
    std::optional<HarmonicReport> harmonic;
    std::optional<EnvelopeReport> envelope;
    std::optional<StereoReport> stereo;
};

Result<Report> analyze(const Signal& signal, const Options& options = {});
// "schema": "supple.analysis-report/1.0"; values rounded per §8.
json::Value toJson(const Report& report, Detail detail = Detail::Summary);
std::string summarize(const Report& report);  // one paragraph, ≤ 80 words

// Unrounded scalar values keyed by dotted report path
// ("loudness.integrated_lufs", "spectral.centroid_hz.median", …).
std::map<std::string, double> flatten(const Report& report);

// --- comparison, profiles, masking (§6) ----------------------------------------------------

struct DescriptorDelta {
    std::string key;
    double reference = 0.0, test = 0.0, delta = 0.0;
    std::string unit;
    bool notable = false;
};
struct Comparison {
    std::vector<DescriptorDelta> deltas;  // notable first, then by key
    std::array<double, kThirdOctaves> ltasDeltaDb{};
    double ltasRmsDb = 0.0;  // after removing the mean offset
};
Comparison compare(const Report& reference, const Report& test);
json::Value toJson(const Comparison& c);
std::string summarize(const Comparison& c);

struct TargetResult {
    std::string key;
    double value = 0.0;
    bool pass = false;
    double distance = 0.0;  // |x − target| / tolerance, or outside-range distance / range
};
struct ProfileResult {
    std::vector<TargetResult> targets;
    double loss = 0.0;  // weighted mean of distances (incl. the LTAS term)
    bool pass = false;
};
// profile: {"schema": "supple.analysis-profile/1.0", "targets": {...}, "ltas"?: [31]}
Result<ProfileResult> evaluate(const Report& report, const json::Value& profile);
json::Value toJson(const ProfileResult& r);

struct MaskingBand {
    double lowHz = 0.0, highHz = 0.0;
    double maskedPct = 0.0;     // of the maskee's active frames
    double meanExcessDb = 0.0;  // threshold above maskee, over masked frames
};
struct MaskingReport {
    std::vector<MaskingBand> bands;  // 24 Bark bands
    double maskedEnergyPct = 0.0;
    std::vector<int> worst;  // indices of the three worst bands
};
Result<MaskingReport> masking(const Signal& masker, const Signal& maskee);
json::Value toJson(const MaskingReport& m);

}  // namespace supple::analysis
