#pragma once

#include <cmath>
#include <limits>

#include "supple/model/session.h"

namespace supple::model {
// Reserve room for uint32 block/latency counts and render-ahead additions.
inline constexpr long double kSampleLimit =
    static_cast<long double>(std::numeric_limits<std::int64_t>::max()) -
    2.L * std::numeric_limits<std::uint32_t>::max() - 65536;
inline Error numericError(std::string path) {
    return Error("time.range", "value or its derived result exceeds the finite engine range",
                 std::move(path), "use a smaller finite value or shorten the range");
}
template <class Number>
inline Status checkedSamples(Number seconds, double rate, const std::string& path) {
    const long double samples = static_cast<long double>(seconds) * static_cast<long double>(rate);
    if (!std::isfinite(seconds) || !std::isfinite(rate) || !(rate > 0) || !std::isfinite(samples) ||
        std::abs(samples) > kSampleLimit)
        return numericError(path);
    return {};
}
inline Status checkedBeat(const Session& s, double beat, const std::string& path, double rate = 0) {
    if (!std::isfinite(beat) || beat < 0 ||
        beat > s.timeMap->beatOfBar(std::numeric_limits<int>::max()))
        return numericError(path);
    return checkedSamples(s.timeMap->secondsAt(beat), rate == 0 ? s.project->sampleRate : rate,
                          path);
}
inline Status checkedSeconds(const Session& s, double seconds, const std::string& path,
                             double rate = 0) {
    SUPPLE_TRY(checkedSamples(seconds, rate == 0 ? s.project->sampleRate : rate, path));
    return checkedBeat(s, s.timeMap->beatAt(seconds), path, rate);
}
Status validateNumeric(const Session& s);
}  // namespace supple::model
