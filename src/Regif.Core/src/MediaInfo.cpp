#include <regif/MediaInfo.h>

#include <cmath>
#include <format>

namespace regif {

std::string_view toString(MediaKind kind)
{
    return kind == MediaKind::Gif ? "GIF" : "Video";
}

std::string formatBytes(std::uintmax_t bytes)
{
    const double b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024.0 * 1024.0) return std::format("{:.2f} GB", b / (1024.0 * 1024.0 * 1024.0));
    if (b >= 1024.0 * 1024.0) return std::format("{:.1f} MB", b / (1024.0 * 1024.0));
    if (b >= 1024.0) return std::format("{:.0f} KB", b / 1024.0);
    return std::format("{} bytes", bytes);
}

std::string summarize(const MediaInfo& info)
{
    std::string text = std::format("{}, {}x{}", toString(info.kind), info.width, info.height);
    if (info.durationSec > 0.0) text += std::format(", {:.2f} s", info.durationSec);
    if (info.frameRate > 0.0) {
        const double rounded = std::round(info.frameRate);
        text += std::abs(info.frameRate - rounded) < 0.01
            ? std::format(", {:.0f} fps", rounded)
            : std::format(", {:.2f} fps", info.frameRate);
    }
    if (info.fileSizeBytes > 0) text += ", " + formatBytes(info.fileSizeBytes);
    return text;
}

} // namespace regif
