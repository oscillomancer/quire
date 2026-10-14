// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Shared command-facing analysis types and conversion for sandbox and engine.
#pragma once

#include <algorithm>
#include <optional>
#include <string>
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

// The analysis signal of decoded audio, by channel count: mono, stereo, 5.0
// or 5.1 (WAV order: L R C [LFE] Ls Rs). Other counts are refused, never
// silently cut to their first two channels. `ptr` holds the channel pointers.
inline Result<analysis::Signal> signalOf(const audio::AudioData& data,
                                         std::vector<const float*>& ptr) {
    ptr.clear();
    for (const auto& c : data.channels) ptr.push_back(c.data());
    analysis::Layout layout = analysis::Layout::Stereo;
    switch (ptr.size()) {
        case 1:
            layout = analysis::Layout::Mono;
            break;
        case 2:
            layout = analysis::Layout::Stereo;
            break;
        case 5:
            layout = analysis::Layout::Surround50;
            break;
        case 6:
            layout = analysis::Layout::Surround51;
            break;
        default:
            return Error("analysis.layout",
                         std::to_string(ptr.size()) + "-channel audio has no analysis layout", {},
                         "analyze mono, stereo, 5.0 or 5.1 audio (or render a stereo mix)");
    }
    return analysis::Signal{
        std::span<const float* const>(ptr.data(), ptr.size()),
        data.channels.empty() ? 0 : static_cast<std::int64_t>(data.channels[0].size()),
        data.sampleRate, layout};
}

struct Loaded {
    audio::AudioData data;
    std::vector<const float*> ptr;
    std::optional<double> noteOff;
    Result<analysis::Signal> signal() { return signalOf(data, ptr); }
};

inline Result<analysis::Report> analyze(Loaded& l) {
    analysis::Options options;
    options.noteOffSeconds = l.noteOff;
    auto signal = l.signal();
    if (!signal) return std::move(signal).error();
    return analysis::analyze(*signal, options);
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
