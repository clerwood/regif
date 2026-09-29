#pragma once

#include <regif/MediaProcessor.h>

namespace regif {

// IMediaProcessor on top of the FFmpeg libraries (libavformat/libavcodec/libavfilter).
// Stateless and safe to use from any thread; each call opens its own contexts.
class FfmpegProcessor final : public IMediaProcessor {
public:
    FfmpegProcessor();

    MediaInfo probe(const std::filesystem::path& file) override;

    void render(const std::filesystem::path& input,
                const MediaInfo& inputInfo,
                const RenderPlan& plan,
                const std::filesystem::path& output,
                const ProgressCallback& progress,
                const CancellationToken& cancel) override;
};

} // namespace regif
