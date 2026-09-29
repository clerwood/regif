#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace regif {

enum class MediaKind { Video, Gif };

// Everything regif needs to know about a file to validate and plan operations.
// Dimensions are as displayed, i.e. after any rotation stored in the container.
struct MediaInfo {
    MediaKind kind = MediaKind::Video;
    int width = 0;
    int height = 0;
    double durationSec = 0.0;      // 0 when unknown
    double frameRate = 0.0;        // average frames per second, 0 when unknown
    std::int64_t frameCount = 0;   // 0 when unknown
    int rotationDegrees = 0;       // clockwise rotation a player applies: 0, 90, 180 or 270
    std::uintmax_t fileSizeBytes = 0;
};

std::string_view toString(MediaKind kind);

// One-line human readable summary, e.g. "GIF, 480x270, 3.20 s, 15 fps, 1.2 MB".
std::string summarize(const MediaInfo& info);

std::string formatBytes(std::uintmax_t bytes);

} // namespace regif
