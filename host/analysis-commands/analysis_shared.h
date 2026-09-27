// Shared command-facing analysis types and conversion for sandbox and engine.
#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "supple/analysis/analysis.h"
#include "supple/audio/audio_file.h"
#include "supple/schema/schema.h"

namespace supple::analysis_commands {

enum class DetailArg { Summary, Standard, Full };

inline analysis::Detail toDetail(DetailArg d) {
    return d == DetailArg::Full       ? analysis::Detail::Full
           : d == DetailArg::Standard ? analysis::Detail::Standard
                                      : analysis::Detail::Summary;
}

inline schema::Schema<DetailArg> detailSchema() {
    return schema::enumeration<DetailArg>({{DetailArg::Summary, "summary"},
                                           {DetailArg::Standard, "standard"},
                                           {DetailArg::Full, "full"}})
        .doc("summary: key scalars; standard: + percentiles and spectrum; full: + time series.");
}

struct Loaded {
    audio::AudioData data;
    std::vector<const float*> ptr;
    std::optional<double> noteOff;
    analysis::Signal signal() {
        ptr.clear();
        for (const auto& c : data.channels) ptr.push_back(c.data());
        const auto layout = ptr.size() >= 2 ? analysis::Layout::Stereo : analysis::Layout::Mono;
        return {std::span<const float* const>(ptr.data(), std::min<std::size_t>(ptr.size(), 2)),
                data.channels.empty() ? 0 : static_cast<std::int64_t>(data.channels[0].size()),
                data.sampleRate, layout};
    }
};

inline Result<analysis::Report> analyze(Loaded& l) {
    analysis::Options options;
    options.noteOffSeconds = l.noteOff;
    return analysis::analyze(l.signal(), options);
}

inline json::Value reportJson(const analysis::Report& report, analysis::Detail detail) {
    json::Value value = analysis::toJson(report, detail);
    value["summary"] = analysis::summarize(report);
    return value;
}

struct CompareResult {
    json::Value notable;
    double ltasRmsDb = 0;
    std::string summary;
};

inline CompareResult compare(const analysis::Report& reference, const analysis::Report& test) {
    const auto result = analysis::compare(reference, test);
    const json::Value value = analysis::toJson(result);
    return {*value.find("notable"),
            value.find("ltas_rms_db")->isNumber() ? value.find("ltas_rms_db")->asDouble() : 0.0,
            analysis::summarize(result)};
}

inline schema::Schema<CompareResult> compareSchema() {
    return schema::object<CompareResult>("comparison")
        .field("notable", &CompareResult::notable, schema::any())
        .field("ltas_rms_db", &CompareResult::ltasRmsDb, schema::number())
        .field("summary", &CompareResult::summary, schema::string())
        .build();
}
}  // namespace supple::analysis_commands
