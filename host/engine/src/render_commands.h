#pragma once
#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/cmd/bus.h"
#include "supple/engine/offline.h"
namespace supple::engine {
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
};
// Resamples, downmixes and normalizes `data` in place and returns how to write it.
Result<audio::WavOptions> finishRenderAudio(audio::AudioData& data, const model::Project& project,
                                            const RenderOutputArgs& args);
Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity);
// Adds rendered files (a render result's "files": path, start_beat) to the
// project as items on a new track each, at their start, as one undo step
// right after the render (posted to the model thread; from a job, when it
// completes). Used by add_to_project on the render commands.
void postAddToProject(cmd::Dispatcher& dispatcher, const cmd::Origin& origin,
                      const json::Value& files);
void registerRenderCommands(cmd::Registry& registry);
}  // namespace supple::engine
