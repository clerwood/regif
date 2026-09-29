#pragma once

#include "MediaInfo.h"
#include "Operations.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace regif {

using OptionList = std::vector<std::pair<std::string, std::string>>;

// A backend-neutral description of how to produce the next stage.
// The filter graph uses libavfilter syntax with exactly one unlabeled input and one
// unlabeled output; it is plain data so it can be unit tested without FFmpeg.
struct RenderPlan {
    MediaKind outputKind = MediaKind::Video;
    std::string filterGraph;
    std::string container;       // libavformat muxer name
    std::string encoder;         // libavcodec encoder name
    std::string fileExtension;   // including the dot
    OptionList encoderOptions;
    OptionList muxerOptions;
    double expectedDurationSec = 0.0; // of the output; 0 when unknown. Used for progress only.
};

// Precondition: validate(op, input) returned no error.
RenderPlan planOperation(const Operation& op, const MediaInfo& input);

// Re-encodes `input` as the same kind of stage through the filters in `body`, the way crop,
// trim and cut do: FFV1 for videos, a palette-preserving GIF for GIFs.
RenderPlan planReencode(const MediaInfo& input, std::string_view body);

} // namespace regif
