#pragma once

#include "MediaInfo.h"
#include "Operations.h"
#include "RenderPlan.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace regif {

// Text overlays. They're a live layer on top of the stages rather than a stage of their own:
// Session keeps them in the original file's coordinates and time, maps them through each
// stage's operation for display and editing, and burns them in on export.

struct Rgba {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
    friend bool operator==(const Rgba&, const Rgba&) = default;
};

enum class TextAlign { Left, Center, Right };

struct TextStyle {
    std::string fontFamily = "Segoe UI"; // what the UI shows
    std::string fontFile;                // UTF-8 path FFmpeg loads; resolved by the app from family/bold/italic
    double fontSize = 48.0;              // em size in pixels
    bool bold = true;
    bool italic = false;
    TextAlign align = TextAlign::Center;
    Rgba fill{ 255, 255, 255, 255 };
    Rgba stroke{ 0, 0, 0, 255 };
    double strokeWidth = 3.0;            // pixels; 0 for none
    bool box = false;
    Rgba boxColor{ 0, 0, 0, 160 };
    double boxPadding = 8.0;
    bool shadow = false;
    Rgba shadowColor{ 0, 0, 0, 160 };
    double shadowX = 3.0;
    double shadowY = 3.0;
    friend bool operator==(const TextStyle&, const TextStyle&) = default;
};

// Text on screen from startSec (inclusive) to endSec (exclusive). (x, y) is the anchor: the
// top of the text block, and its left edge, centre or right edge depending on style.align.
struct TextClip {
    std::uint64_t id = 0;
    std::string text; // UTF-8; '\n' separates lines
    double startSec = 0.0;
    double endSec = 0.0;
    double x = 0.0;
    double y = 0.0;
    TextStyle style;
};

// A lane on the timeline. Later tracks draw on top of earlier ones.
struct TextTrack {
    std::uint64_t id = 0;
    std::string name;
    std::vector<TextClip> clips;
};

struct TextLayer {
    std::vector<TextTrack> tracks;

    // True when there's nothing to draw (no clips with text).
    bool empty() const;
    const TextClip* findClip(std::uint64_t clipId) const;
};

inline bool isVisibleAt(const TextClip& clip, double seconds)
{
    return seconds >= clip.startSec && seconds < clip.endSec;
}

// How a clip looks after `op` turned a stage described by `before` into one described by
// `after`: crops move it, trims/cuts/speed changes retime it, resizes scale it. Empty when
// the edit removed the clip's whole time range.
std::optional<TextClip> mapClipForward(const TextClip& clip, const Operation& op, const MediaInfo& before,
                                       const MediaInfo& after);

// The inverse: a clip edited in the `after` stage, expressed in the `before` stage.
TextClip mapClipBackward(const TextClip& clip, const Operation& op, const MediaInfo& before, const MediaInfo& after);

// mapClipForward for every clip. Tracks are kept even when they end up empty.
TextLayer mapLayerForward(const TextLayer& layer, const Operation& op, const MediaInfo& before,
                          const MediaInfo& after);

// Escapes a filter option value for use inside a filter graph string (the option level and
// the graph level, see "Notes on filtergraph escaping" in the FFmpeg docs).
std::string escapeFilterValue(std::string_view value);

// A drawtext filter for one clip. anchorX/anchorY are the anchor's position (numbers or
// drawtext expressions). With `timed`, the text only shows during the clip's time range.
// Shared by export and the preview renderer so both draw the same thing.
std::string drawTextFilter(const TextClip& clip, std::string_view anchorX, std::string_view anchorY, bool timed);

// Re-encodes `stage` with every clip of `layer` drawn on it. Precondition: !layer.empty().
RenderPlan planBurnIn(const TextLayer& layer, const MediaInfo& stage);

} // namespace regif
