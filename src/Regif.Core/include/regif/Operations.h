#pragma once

#include "MediaInfo.h"

#include <optional>
#include <string>
#include <variant>

namespace regif {

enum class Dither { None, Bayer, FloydSteinberg, Sierra2_4a };

struct GifEncodeSettings {
    int maxColors = 256;             // 2..256
    Dither dither = Dither::Sierra2_4a;
    bool perFramePalette = false;    // better quality for busy footage, larger files
};

// Keep only the rectangle (x, y, width, height), in displayed pixels.
struct CropOp {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Keep only [startSec, endSec).
struct TrimOp {
    double startSec = 0.0;
    double endSec = 0.0;
};

// Remove [startSec, endSec) and join what's left.
struct CutOp {
    double startSec = 0.0;
    double endSec = 0.0;
};

// Playback speed multiplier: 2.0 plays twice as fast. GIF only.
struct SpeedOp {
    double factor = 1.0;
};

// Video -> GIF.
struct ConvertToGifOp {
    double fps = 15.0;
    int width = 480;                 // 0 keeps the source width
    GifEncodeSettings gif{};
    bool loop = true;
};

// GIF -> smaller GIF. Zero fps/width keep the current values.
struct OptimizeGifOp {
    double fps = 0.0;
    int width = 0;
    GifEncodeSettings gif{ 128, Dither::Bayer, false };
};

using Operation = std::variant<CropOp, TrimOp, CutOp, SpeedOp, ConvertToGifOp, OptimizeGifOp>;

// Short label used in the stage history, e.g. "Crop to 320x240 at (10, 20)".
std::string describe(const Operation& op);

bool isApplicable(const Operation& op, MediaKind kind);

MediaKind resultKind(const Operation& op, MediaKind input);

// Returns a user-facing explanation when the operation can't be applied to `input`.
std::optional<std::string> validate(const Operation& op, const MediaInfo& input);

std::string_view toString(Dither dither);

} // namespace regif
