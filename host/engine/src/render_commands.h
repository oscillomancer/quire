// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/offline.h"
namespace supple::engine {
class EngineApp;
struct RenderTarget {
    std::uint64_t generation;
    std::string branch;
};
RenderTarget renderTarget(const EngineApp& app);
Status checkRenderTarget(const EngineApp& app, const RenderTarget& target);
// Final publication runs on the model thread. Stale jobs retain their files
// in the result with status/error render.stale_target and never import.
void completeRenderJob(EngineApp& app, cmd::Dispatcher& dispatcher, cmd::JobManager& jobs,
                       std::string id, RenderTarget target, json::Value result,
                       std::function<void()> publish);
struct RenderPatternContext {
    std::string project, track, folderPath, region, marker, item, take, format = "wav";
    int trackNumber = 0, regionNumber = 0, sampleRate = 48000, bitDepth = 32, counter = 1;
    double startSeconds = 0, startBeats = 0;
};
Result<std::string> expandRenderPattern(std::string_view pattern,
                                        const RenderPatternContext& context);
Status normalizeRenderAudio(audio::AudioData& data, std::string_view mode, double target,
                            std::optional<double> limitTruePeakDb = {});
// The output side of every render command: overrides from the command, else the
// project's render settings (session-model §1.10).
struct RenderOutputArgs {
    std::optional<std::string> format;
    std::optional<json::Value> normalize;
    std::optional<double> target, limitTruePeakDb;
    std::optional<std::int64_t> bitDepth, sampleRate, channels;
    std::optional<json::Value> secondary;  // {format, bit_depth} | "none"
};
// How a render file is written: the container (wav | flac) and its sample
// format; `wav` carries the format, dither and seed for both containers.
struct RenderEncoding {
    std::string container = "wav";
    audio::WavOptions wav;
    int bits() const;               // 16, 24 or 32 (float, WAV only)
    std::string extension() const;  // ".wav" / ".flac"
};
// The primary encoding from the command's format/bit_depth, else the project's
// render settings (FLAC defaults to 24 bits, WAV to 32-bit float).
Result<RenderEncoding> renderEncoding(const model::Project& project,
                                      const std::optional<std::string>& format,
                                      std::optional<std::int64_t> bitDepth);
// A second file of the same audio (session-model §1.10 `secondary`), if any.
Result<std::optional<RenderEncoding>> secondaryEncoding(
    const model::Project& project, const std::optional<json::Value>& secondary);
// Beside the primary file: its stem with the secondary's extension, and the
// bit depth before it when both are the same container ("mix.16.wav").
std::filesystem::path secondaryPath(const std::filesystem::path& primary,
                                    const RenderEncoding& first, const RenderEncoding& second);
Status writeRender(const std::filesystem::path& path, const audio::AudioData& data,
                   const RenderEncoding& encoding);
// What finishRenderAudio decided: the file encodings.
struct RenderOutput {
    RenderEncoding primary;
    std::optional<RenderEncoding> secondary;
};
// Resamples, downmixes and normalizes `data` in place and returns how to write it.
Result<RenderOutput> finishRenderAudio(audio::AudioData& data, const model::Project& project,
                                       const RenderOutputArgs& args);
Status checkRenderRange(const model::Session& session, const OfflineSettings& settings,
                        std::optional<std::int64_t> sampleRate = {});
Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity);
// Adds rendered files (a render result's "files": path, start_beat) to the
// project as items on a new track each, at their start, as one undo step
// right after the render (posted to the model thread; from a job, when it
// completes). Used by add_to_project on the render commands.
void postAddToProject(EngineApp& app, RenderTarget target, cmd::Dispatcher& dispatcher,
                      const cmd::Origin& origin, const json::Value& files);
void registerRenderCommands(cmd::Registry& registry);
}  // namespace supple::engine
