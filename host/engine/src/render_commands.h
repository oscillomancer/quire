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
Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity);
void registerRenderCommands(cmd::Registry& registry);
}  // namespace supple::engine
