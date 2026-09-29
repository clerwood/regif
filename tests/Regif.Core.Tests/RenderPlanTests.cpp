#include "TestFramework.h"

#include <regif/RenderPlan.h>

#include <algorithm>

using namespace regif;

namespace {

MediaInfo video(int rotation = 0)
{
    MediaInfo info;
    info.kind = MediaKind::Video;
    info.width = 1280;
    info.height = 720;
    info.durationSec = 10.0;
    info.frameRate = 30.0;
    info.rotationDegrees = rotation;
    return info;
}

MediaInfo gif(double fps = 15.0)
{
    MediaInfo info;
    info.kind = MediaKind::Gif;
    info.width = 480;
    info.height = 270;
    info.durationSec = 4.0;
    info.frameRate = fps;
    return info;
}

std::string option(const OptionList& options, const std::string& key)
{
    const auto it = std::find_if(options.begin(), options.end(), [&](const auto& kv) { return kv.first == key; });
    return it == options.end() ? std::string("<missing>") : it->second;
}

const std::string kPreservePalette =
    "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors=256:stats_mode=full[rg_pal];"
    "[rg_src][rg_pal]paletteuse=dither=none:diff_mode=rectangle";

} // namespace

TEST_CASE("video edits stay lossless FFV1 in Matroska")
{
    const RenderPlan plan = planOperation(CropOp{ 11, 21, 301, 201 }, video());
    CHECK(plan.outputKind == MediaKind::Video);
    CHECK_EQ(plan.container, std::string("matroska"));
    CHECK_EQ(plan.encoder, std::string("ffv1"));
    CHECK_EQ(plan.fileExtension, std::string(".mkv"));
    CHECK_EQ(option(plan.encoderOptions, "level"), std::string("3"));
    CHECK(plan.muxerOptions.empty());
    CHECK_EQ(plan.filterGraph, std::string("format=yuv444p,crop=w=301:h=201:x=11:y=21:exact=1"));
}

TEST_CASE("rotated source videos are turned upright first")
{
    CHECK_EQ(planOperation(TrimOp{ 1, 2 }, video(90)).filterGraph,
             std::string("transpose=clock,format=yuv444p,trim=start=1:end=2,setpts=PTS-STARTPTS"));
    CHECK_EQ(planOperation(TrimOp{ 1, 2 }, video(180)).filterGraph,
             std::string("hflip,vflip,format=yuv444p,trim=start=1:end=2,setpts=PTS-STARTPTS"));
    CHECK_EQ(planOperation(TrimOp{ 1, 2 }, video(270)).filterGraph,
             std::string("transpose=cclock,format=yuv444p,trim=start=1:end=2,setpts=PTS-STARTPTS"));

    const std::string convert = planOperation(ConvertToGifOp{}, video(90)).filterGraph;
    CHECK_CONTAINS(convert, "transpose=clock,fps=15,scale=480:-1:flags=lanczos,split");
}

TEST_CASE("trim uses locale-independent decimals")
{
    CHECK_EQ(planOperation(TrimOp{ 1.25, 3.5 }, video()).filterGraph,
             std::string("format=yuv444p,trim=start=1.25:end=3.5,setpts=PTS-STARTPTS"));
    CHECK_EQ(planOperation(TrimOp{ 0.1, 0.333333333 }, video()).filterGraph,
             std::string("format=yuv444p,trim=start=0.1:end=0.333333,setpts=PTS-STARTPTS"));
}

TEST_CASE("cut in the middle splits and concatenates")
{
    CHECK_EQ(planOperation(CutOp{ 2, 3.5 }, video()).filterGraph,
             std::string("format=yuv444p,split[rc_a][rc_b];"
                         "[rc_a]trim=end=2,setpts=PTS-STARTPTS[rc_head];"
                         "[rc_b]trim=start=3.5,setpts=PTS-STARTPTS[rc_tail];"
                         "[rc_head][rc_tail]concat=n=2:v=1:a=0"));
}

TEST_CASE("cut at either edge is a plain trim")
{
    CHECK_EQ(planOperation(CutOp{ 0, 2 }, video()).filterGraph,
             std::string("format=yuv444p,trim=start=2,setpts=PTS-STARTPTS"));
    CHECK_EQ(planOperation(CutOp{ 8, 10 }, video()).filterGraph,
             std::string("format=yuv444p,trim=end=8,setpts=PTS-STARTPTS"));
    CHECK_EQ(planOperation(CutOp{ 8, 15 }, video()).filterGraph,
             std::string("format=yuv444p,trim=end=8,setpts=PTS-STARTPTS"));
}

TEST_CASE("GIF edits re-encode without adding dither")
{
    const RenderPlan plan = planOperation(CropOp{ 0, 0, 100, 100 }, gif());
    CHECK(plan.outputKind == MediaKind::Gif);
    CHECK_EQ(plan.container, std::string("gif"));
    CHECK_EQ(plan.encoder, std::string("gif"));
    CHECK_EQ(plan.fileExtension, std::string(".gif"));
    CHECK_EQ(option(plan.muxerOptions, "loop"), std::string("0"));
    CHECK_EQ(plan.filterGraph, "format=bgra,crop=w=100:h=100:x=0:y=0:exact=1," + kPreservePalette);
}

TEST_CASE("speed changes timestamps and caps the frame rate at 50 fps")
{
    CHECK_EQ(planOperation(SpeedOp{ 2 }, gif(15)).filterGraph, "format=bgra,setpts=PTS/2," + kPreservePalette);
    CHECK_EQ(planOperation(SpeedOp{ 0.5 }, gif(15)).filterGraph, "format=bgra,setpts=PTS/0.5," + kPreservePalette);
    CHECK_EQ(planOperation(SpeedOp{ 4 }, gif(25)).filterGraph, "format=bgra,setpts=PTS/4,fps=50," + kPreservePalette);
}

TEST_CASE("convert to GIF builds a diff palette")
{
    ConvertToGifOp op;
    op.fps = 12.5;
    op.width = 320;
    op.gif = { 128, Dither::Bayer, false };
    const RenderPlan plan = planOperation(op, video());
    CHECK(plan.outputKind == MediaKind::Gif);
    CHECK_EQ(plan.filterGraph,
             std::string("fps=12.5,scale=320:-1:flags=lanczos,"
                         "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors=128:stats_mode=diff[rg_pal];"
                         "[rg_src][rg_pal]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle"));

    op.loop = false;
    op.width = 0;
    op.gif = { 256, Dither::Sierra2_4a, true };
    const RenderPlan once = planOperation(op, video());
    CHECK_EQ(option(once.muxerOptions, "loop"), std::string("-1"));
    CHECK_EQ(once.filterGraph,
             std::string("fps=12.5,"
                         "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors=256:stats_mode=single[rg_pal];"
                         "[rg_src][rg_pal]paletteuse=dither=sierra2_4a:diff_mode=rectangle:new=1"));
}

TEST_CASE("optimize only adds the filters it needs")
{
    CHECK_EQ(planOperation(OptimizeGifOp{}, gif()).filterGraph,
             std::string("format=bgra,"
                         "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors=128:stats_mode=diff[rg_pal];"
                         "[rg_src][rg_pal]paletteuse=dither=bayer:bayer_scale=3:diff_mode=rectangle"));

    OptimizeGifOp op;
    op.fps = 10;
    op.width = 240;
    op.gif = { 64, Dither::FloydSteinberg, false };
    CHECK_EQ(planOperation(op, gif()).filterGraph,
             std::string("format=bgra,fps=10,scale=240:-1:flags=lanczos,"
                         "split[rg_src][rg_pal_in];[rg_pal_in]palettegen=max_colors=64:stats_mode=diff[rg_pal];"
                         "[rg_src][rg_pal]paletteuse=dither=floyd_steinberg:diff_mode=rectangle"));
}

TEST_CASE("plans estimate the output duration for progress reporting")
{
    CHECK_EQ(planOperation(CropOp{ 0, 0, 10, 10 }, video()).expectedDurationSec, 10.0);
    CHECK_EQ(planOperation(TrimOp{ 2, 5 }, video()).expectedDurationSec, 3.0);
    CHECK_EQ(planOperation(TrimOp{ 8, 20 }, video()).expectedDurationSec, 2.0);
    CHECK_EQ(planOperation(CutOp{ 2, 5 }, video()).expectedDurationSec, 7.0);
    CHECK_EQ(planOperation(SpeedOp{ 2 }, gif()).expectedDurationSec, 2.0);
    MediaInfo unknown = video();
    unknown.durationSec = 0;
    CHECK_EQ(planOperation(TrimOp{ 2, 5 }, unknown).expectedDurationSec, 0.0);
}
