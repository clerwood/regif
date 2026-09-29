#include <regif/Operations.h>

#include <cmath>
#include <format>

namespace regif {
namespace {

template <class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string seconds(double s) { return std::format("{:.2f}s", s); }

constexpr double kMaxGifFps = 50.0; // browsers clamp shorter frame delays, see RenderPlan.cpp
constexpr int kMinWidth = 16;

std::optional<std::string> validateColors(int colors)
{
    if (colors < 2 || colors > 256) return "The number of colors must be between 2 and 256.";
    return std::nullopt;
}

std::optional<std::string> validateRange(double start, double end, const MediaInfo& in, bool removing)
{
    if (start < 0.0) return "The start time can't be negative.";
    if (end <= start) return "The end time must be after the start time.";
    if (in.durationSec > 0.0) {
        if (start >= in.durationSec)
            return std::format("The start time is past the end of the clip ({}).", seconds(in.durationSec));
        if (start <= 0.0 && end >= in.durationSec)
            return removing ? std::string("That range covers the whole clip, so nothing would be left.")
                            : std::string("That range keeps the whole clip, so there's nothing to trim.");
    }
    return std::nullopt;
}

} // namespace

std::string_view toString(Dither dither)
{
    switch (dither) {
    case Dither::None: return "no dither";
    case Dither::Bayer: return "Bayer dither";
    case Dither::FloydSteinberg: return "Floyd-Steinberg dither";
    case Dither::Sierra2_4a: return "Sierra 2-4A dither";
    }
    return "dither";
}

std::string describe(const Operation& op)
{
    return std::visit(Overloaded{
        [](const CropOp& c) {
            return std::format("Crop to {}x{} at ({}, {})", c.width, c.height, c.x, c.y);
        },
        [](const TrimOp& t) {
            return std::format("Trim to {} - {}", seconds(t.startSec), seconds(t.endSec));
        },
        [](const CutOp& c) {
            return std::format("Cut out {} - {}", seconds(c.startSec), seconds(c.endSec));
        },
        [](const SpeedOp& s) {
            return std::format("Change speed to {:g}x", s.factor);
        },
        [](const ConvertToGifOp& g) {
            const std::string size = g.width > 0 ? std::format("{} px wide", g.width) : std::string("original size");
            return std::format("Convert to GIF ({:g} fps, {}, {} colors)", g.fps, size, g.gif.maxColors);
        },
        [](const OptimizeGifOp& o) {
            std::string text = std::format("Optimize GIF ({} colors, {}", o.gif.maxColors, toString(o.gif.dither));
            if (o.fps > 0.0) text += std::format(", {:g} fps", o.fps);
            if (o.width > 0) text += std::format(", {} px wide", o.width);
            return text + ")";
        },
    }, op);
}

bool isApplicable(const Operation& op, MediaKind kind)
{
    return std::visit(Overloaded{
        [](const CropOp&) { return true; },
        [](const TrimOp&) { return true; },
        [](const CutOp&) { return true; },
        [kind](const SpeedOp&) { return kind == MediaKind::Gif; },
        [kind](const ConvertToGifOp&) { return kind == MediaKind::Video; },
        [kind](const OptimizeGifOp&) { return kind == MediaKind::Gif; },
    }, op);
}

MediaKind resultKind(const Operation& op, MediaKind input)
{
    return std::holds_alternative<ConvertToGifOp>(op) ? MediaKind::Gif : input;
}

std::optional<std::string> validate(const Operation& op, const MediaInfo& in)
{
    if (!isApplicable(op, in.kind)) {
        return std::holds_alternative<ConvertToGifOp>(op)
            ? std::string("This is already a GIF.")
            : std::string("This change only works on GIFs. Convert the video to a GIF first.");
    }

    return std::visit(Overloaded{
        [&](const CropOp& c) -> std::optional<std::string> {
            if (c.width < 1 || c.height < 1) return "The crop width and height must be at least 1 pixel.";
            if (c.x < 0 || c.y < 0) return "The crop position can't be negative.";
            if (c.x + c.width > in.width || c.y + c.height > in.height)
                return std::format("The crop rectangle goes past the edge of the {}x{} frame.", in.width, in.height);
            if (c.x == 0 && c.y == 0 && c.width == in.width && c.height == in.height)
                return "The crop rectangle covers the whole frame, so there's nothing to crop.";
            return std::nullopt;
        },
        [&](const TrimOp& t) { return validateRange(t.startSec, t.endSec, in, false); },
        [&](const CutOp& c) { return validateRange(c.startSec, c.endSec, in, true); },
        [&](const SpeedOp& s) -> std::optional<std::string> {
            if (!(s.factor >= 0.1 && s.factor <= 10.0)) return "The speed must be between 0.1x and 10x.";
            if (std::abs(s.factor - 1.0) < 1e-6) return "A speed of 1x wouldn't change anything.";
            return std::nullopt;
        },
        [&](const ConvertToGifOp& g) -> std::optional<std::string> {
            if (!(g.fps >= 1.0 && g.fps <= kMaxGifFps)) return "The GIF frame rate must be between 1 and 50 fps.";
            if (g.width != 0 && g.width < kMinWidth)
                return "The width must be at least 16 pixels, or 0 to keep the original size.";
            return validateColors(g.gif.maxColors);
        },
        [&](const OptimizeGifOp& o) -> std::optional<std::string> {
            if (auto problem = validateColors(o.gif.maxColors)) return problem;
            if (o.fps != 0.0) {
                if (!(o.fps >= 1.0 && o.fps <= kMaxGifFps)) return "The frame rate must be between 1 and 50 fps, or 0 to keep it.";
                if (in.frameRate > 0.0 && o.fps > in.frameRate + 0.01)
                    return std::format("Optimizing can only lower the frame rate (currently {:.2f} fps).", in.frameRate);
            }
            if (o.width != 0) {
                if (o.width < kMinWidth) return "The width must be at least 16 pixels, or 0 to keep it.";
                if (o.width > in.width)
                    return std::format("Optimizing can only shrink the GIF (currently {} px wide).", in.width);
            }
            return std::nullopt;
        },
    }, op);
}

} // namespace regif
