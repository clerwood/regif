#include <regif/RenderPlan.h>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <string_view>

namespace regif {
namespace {

template <class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// Browsers treat GIF frame delays under 20 ms as 100 ms, so anything faster than 50 fps
// would actually play slower. When a speed-up would cross that line we drop frames instead.
constexpr double kMaxGifFps = 50.0;

// Locale-independent decimal for filter arguments: 1.5 -> "1.5", 2.0 -> "2".
std::string num(double value)
{
    std::string s = std::format("{:.6f}", value);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string chain(std::initializer_list<std::string_view> parts)
{
    std::string out;
    for (std::string_view part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += ',';
        out += part;
    }
    return out;
}

std::string_view ditherFilterName(Dither dither)
{
    switch (dither) {
    case Dither::None: return "none";
    case Dither::Bayer: return "bayer";
    case Dither::FloydSteinberg: return "floyd_steinberg";
    case Dither::Sierra2_4a: return "sierra2_4a";
    }
    return "sierra2_4a";
}

// Only the original file can carry a rotation; regif's own stage files are written upright.
std::string rotationFilter(const MediaInfo& in)
{
    if (in.kind != MediaKind::Video) return {};
    switch (in.rotationDegrees) {
    case 90: return "transpose=clock";
    case 180: return "hflip,vflip";
    case 270: return "transpose=cclock";
    default: return {};
    }
}

// Single-pass palette generation: palettegen sees every frame, then paletteuse maps them.
// `statsMode` is "full" when re-encoding an existing GIF (keep its colors) and "diff" for
// new or optimized GIFs (spend the palette on what moves).
std::string paletteFilters(const GifEncodeSettings& s, std::string_view statsMode)
{
    std::string use = std::format("paletteuse=dither={}", ditherFilterName(s.dither));
    if (s.dither == Dither::Bayer) use += ":bayer_scale=3";
    use += ":diff_mode=rectangle";
    if (s.perFramePalette) use += ":new=1";

    return std::format(
        "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors={}:stats_mode={}[rg_pal];[rg_src][rg_pal]{}",
        s.maxColors, s.perFramePalette ? std::string_view("single") : statsMode, use);
}

// Re-encoding a GIF after crop/trim/cut/speed: keep up to 256 existing colors, don't dither.
constexpr GifEncodeSettings kPreserveGif{ 256, Dither::None, false };

RenderPlan videoPlan(const MediaInfo& in, std::string_view body)
{
    RenderPlan plan;
    plan.outputKind = MediaKind::Video;
    // yuv444p before the edit keeps odd crop offsets exact and avoids re-subsampling
    // chroma at every stage; FFV1 then stores each stage losslessly.
    plan.filterGraph = chain({ rotationFilter(in), "format=yuv444p", body });
    plan.container = "matroska";
    plan.encoder = "ffv1";
    plan.fileExtension = ".mkv";
    plan.encoderOptions = { { "level", "3" } };
    return plan;
}

RenderPlan gifPlan(const MediaInfo& in, std::string_view body, const GifEncodeSettings& gif,
                   std::string_view statsMode, bool loop)
{
    RenderPlan plan;
    plan.outputKind = MediaKind::Gif;
    const std::string source = in.kind == MediaKind::Gif ? std::string("format=bgra") : rotationFilter(in);
    plan.filterGraph = chain({ source, body, paletteFilters(gif, statsMode) });
    plan.container = "gif";
    plan.encoder = "gif";
    plan.fileExtension = ".gif";
    plan.muxerOptions = { { "loop", loop ? "0" : "-1" } };
    return plan;
}

RenderPlan sameKindPlan(const MediaInfo& in, std::string_view body)
{
    return in.kind == MediaKind::Gif ? gifPlan(in, body, kPreserveGif, "full", true) : videoPlan(in, body);
}

double expectedDuration(const Operation& op, const MediaInfo& in)
{
    const double d = in.durationSec;
    if (d <= 0.0) return 0.0;
    return std::visit(Overloaded{
        [d](const TrimOp& t) { return std::max(0.0, std::min(t.endSec, d) - t.startSec); },
        [d](const CutOp& c) { return std::max(0.0, d - (std::min(c.endSec, d) - c.startSec)); },
        [d](const SpeedOp& s) { return d / s.factor; },
        [d](const auto&) { return d; },
    }, op);
}

RenderPlan planOperationImpl(const Operation& op, const MediaInfo& in)
{
    return std::visit(Overloaded{
        [&](const CropOp& c) {
            return sameKindPlan(in, std::format("crop=w={}:h={}:x={}:y={}:exact=1", c.width, c.height, c.x, c.y));
        },
        [&](const TrimOp& t) {
            return sameKindPlan(in, std::format("trim=start={}:end={},setpts=PTS-STARTPTS", num(t.startSec), num(t.endSec)));
        },
        [&](const CutOp& c) {
            std::string body;
            if (c.startSec <= 0.0) {
                body = std::format("trim=start={},setpts=PTS-STARTPTS", num(c.endSec));
            } else if (in.durationSec > 0.0 && c.endSec >= in.durationSec) {
                body = std::format("trim=end={},setpts=PTS-STARTPTS", num(c.startSec));
            } else {
                body = std::format(
                    "split[rc_a][rc_b];"
                    "[rc_a]trim=end={},setpts=PTS-STARTPTS[rc_head];"
                    "[rc_b]trim=start={},setpts=PTS-STARTPTS[rc_tail];"
                    "[rc_head][rc_tail]concat=n=2:v=1:a=0",
                    num(c.startSec), num(c.endSec));
            }
            return sameKindPlan(in, body);
        },
        [&](const SpeedOp& s) {
            std::string body = std::format("setpts=PTS/{}", num(s.factor));
            if (in.frameRate > 0.0 && in.frameRate * s.factor > kMaxGifFps) body += std::format(",fps={}", num(kMaxGifFps));
            return gifPlan(in, body, kPreserveGif, "full", true);
        },
        [&](const ConvertToGifOp& g) {
            const std::string scale = g.width > 0 ? std::format("scale={}:-1:flags=lanczos", g.width) : std::string();
            return gifPlan(in, chain({ std::format("fps={}", num(g.fps)), scale }), g.gif, "diff", g.loop);
        },
        [&](const OptimizeGifOp& o) {
            const std::string fps = o.fps > 0.0 ? std::format("fps={}", num(o.fps)) : std::string();
            const std::string scale = o.width > 0 ? std::format("scale={}:-1:flags=lanczos", o.width) : std::string();
            return gifPlan(in, chain({ fps, scale }), o.gif, "diff", true);
        },
    }, op);
}

} // namespace

RenderPlan planReencode(const MediaInfo& in, std::string_view body)
{
    RenderPlan plan = sameKindPlan(in, body);
    plan.expectedDurationSec = std::max(0.0, in.durationSec);
    return plan;
}

RenderPlan planOperation(const Operation& op, const MediaInfo& in)
{
    RenderPlan plan = planOperationImpl(op, in);
    plan.expectedDurationSec = expectedDuration(op, in);
    return plan;
}

} // namespace regif
