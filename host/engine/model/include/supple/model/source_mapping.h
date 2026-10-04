// Source beats mapped to item beats; visible loop passes are bounded for drawing.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace supple::model {

inline std::vector<double> visibleLoopPasses(double position, double length, double sourceLength,
                                             bool loop, double from, double to,
                                             std::size_t limit = 8192) {
    std::vector<double> out;
    if (!(sourceLength > 0) || !std::isfinite(sourceLength) || !(length > 0)) return out;
    const double low = std::max(position, from), high = std::min(position + length, to);
    if (high < low) return out;
    if (!loop) {
        out.push_back(position);
        return out;
    }
    const double first = std::max(0.0, std::floor((low - position) / sourceLength));
    const double last = std::floor((high - position) / sourceLength);
    if (!std::isfinite(first) || !std::isfinite(last)) return out;
    double previous = -1;
    for (std::size_t n = 0; n < limit; ++n) {
        const double pass = first + static_cast<double>(n);
        if (pass > last || pass == previous) break;
        const double start = position + pass * sourceLength;
        if (start >= position + length) break;
        out.push_back(start);
        previous = pass;
    }
    return out;
}

struct SourceBeatMapping {
    double offset = 0, rate = 1, sourceLength = 0;
    double period() const { return sourceLength / rate; }
    double sourceOffset(bool loop) const {
        return loop && sourceLength > 0 ? std::fmod(offset, sourceLength) : offset;
    }
    double itemBeat(double sourceBeat, double pass = 0, bool loop = false) const {
        return (sourceBeat + pass * sourceLength - sourceOffset(loop)) / rate;
    }
    std::vector<double> visibleOrigins(double duration, bool loop, double from, double to) const {
        if (!(rate > 0) || !std::isfinite(rate) || !(period() > 0) || !std::isfinite(period()))
            return {};
        const double span = loop ? (1 + std::floor(duration / period())) * period() : period();
        if (!std::isfinite(span)) return {};
        return visibleLoopPasses(itemBeat(0, 0, loop), span, period(), loop, from, to);
    }
};

}  // namespace supple::model
