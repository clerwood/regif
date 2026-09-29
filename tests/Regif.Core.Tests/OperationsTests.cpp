#include "TestFramework.h"

#include <regif/MediaInfo.h>
#include <regif/Operations.h>

using namespace regif;

namespace {

MediaInfo video()
{
    MediaInfo info;
    info.kind = MediaKind::Video;
    info.width = 1920;
    info.height = 1080;
    info.durationSec = 10.0;
    info.frameRate = 30.0;
    return info;
}

MediaInfo gif()
{
    MediaInfo info;
    info.kind = MediaKind::Gif;
    info.width = 480;
    info.height = 270;
    info.durationSec = 4.0;
    info.frameRate = 15.0;
    info.frameCount = 60;
    return info;
}

} // namespace

TEST_CASE("summarize and formatBytes are readable")
{
    MediaInfo info = gif();
    info.fileSizeBytes = 1'258'291;
    CHECK_EQ(summarize(info), std::string("GIF, 480x270, 4.00 s, 15 fps, 1.2 MB"));
    CHECK_EQ(formatBytes(512), std::string("512 bytes"));
    CHECK_EQ(formatBytes(2048), std::string("2 KB"));

    info.frameRate = 29.97;
    CHECK_CONTAINS(summarize(info), "29.97 fps");
}

TEST_CASE("operations are limited to the right media kind")
{
    CHECK(isApplicable(CropOp{}, MediaKind::Video));
    CHECK(isApplicable(CropOp{}, MediaKind::Gif));
    CHECK(isApplicable(TrimOp{}, MediaKind::Video));
    CHECK(isApplicable(CutOp{}, MediaKind::Gif));
    CHECK(!isApplicable(SpeedOp{}, MediaKind::Video));
    CHECK(isApplicable(SpeedOp{}, MediaKind::Gif));
    CHECK(isApplicable(ConvertToGifOp{}, MediaKind::Video));
    CHECK(!isApplicable(ConvertToGifOp{}, MediaKind::Gif));
    CHECK(!isApplicable(OptimizeGifOp{}, MediaKind::Video));
    CHECK(isApplicable(OptimizeGifOp{}, MediaKind::Gif));

    CHECK(resultKind(ConvertToGifOp{}, MediaKind::Video) == MediaKind::Gif);
    CHECK(resultKind(CropOp{}, MediaKind::Video) == MediaKind::Video);
    CHECK(resultKind(SpeedOp{}, MediaKind::Gif) == MediaKind::Gif);
}

TEST_CASE("validate explains why an operation doesn't fit")
{
    CHECK_CONTAINS(*validate(SpeedOp{ 2.0 }, video()), "only works on GIFs");
    CHECK_CONTAINS(*validate(ConvertToGifOp{}, gif()), "already a GIF");
}

TEST_CASE("crop validation")
{
    CHECK(!validate(CropOp{ 10, 20, 320, 240 }, video()));
    CHECK(!validate(CropOp{ 1, 1, 1919, 1079 }, video()));
    CHECK(validate(CropOp{ 0, 0, 0, 240 }, video()).has_value());
    CHECK(validate(CropOp{ -1, 0, 100, 100 }, video()).has_value());
    CHECK_CONTAINS(*validate(CropOp{ 1800, 0, 200, 100 }, video()), "past the edge");
    CHECK_CONTAINS(*validate(CropOp{ 0, 0, 1920, 1080 }, video()), "whole frame");
}

TEST_CASE("trim and cut validation")
{
    CHECK(!validate(TrimOp{ 1.0, 5.0 }, video()));
    CHECK(!validate(TrimOp{ 0.0, 5.0 }, video()));
    CHECK(!validate(TrimOp{ 5.0, 20.0 }, video())); // end past the clip is clamped by the trim filter
    CHECK(validate(TrimOp{ -1.0, 5.0 }, video()).has_value());
    CHECK(validate(TrimOp{ 5.0, 5.0 }, video()).has_value());
    CHECK(validate(TrimOp{ 11.0, 12.0 }, video()).has_value());
    CHECK_CONTAINS(*validate(TrimOp{ 0.0, 10.0 }, video()), "nothing to trim");

    CHECK(!validate(CutOp{ 2.0, 3.0 }, gif()));
    CHECK_CONTAINS(*validate(CutOp{ 0.0, 4.0 }, gif()), "nothing would be left");
    CHECK_CONTAINS(*validate(CutOp{ 3.0, 1.0 }, gif()), "after the start");
}

TEST_CASE("speed validation")
{
    CHECK(!validate(SpeedOp{ 2.0 }, gif()));
    CHECK(!validate(SpeedOp{ 0.5 }, gif()));
    CHECK(validate(SpeedOp{ 0.05 }, gif()).has_value());
    CHECK(validate(SpeedOp{ 11.0 }, gif()).has_value());
    CHECK_CONTAINS(*validate(SpeedOp{ 1.0 }, gif()), "wouldn't change");
}

TEST_CASE("convert and optimize validation")
{
    CHECK(!validate(ConvertToGifOp{}, video()));
    ConvertToGifOp convert;
    convert.fps = 60.0;
    CHECK(validate(convert, video()).has_value());
    convert = ConvertToGifOp{};
    convert.width = 8;
    CHECK(validate(convert, video()).has_value());
    convert.width = 0;
    CHECK(!validate(convert, video()));
    convert.gif.maxColors = 1;
    CHECK_CONTAINS(*validate(convert, video()), "between 2 and 256");

    CHECK(!validate(OptimizeGifOp{}, gif()));
    OptimizeGifOp optimize;
    optimize.fps = 10.0;
    optimize.width = 320;
    CHECK(!validate(optimize, gif()));
    optimize.fps = 30.0;
    CHECK_CONTAINS(*validate(optimize, gif()), "only lower the frame rate");
    optimize.fps = 0.0;
    optimize.width = 640;
    CHECK_CONTAINS(*validate(optimize, gif()), "only shrink");
}

TEST_CASE("describe produces history labels")
{
    CHECK_EQ(describe(CropOp{ 10, 20, 320, 240 }), std::string("Crop to 320x240 at (10, 20)"));
    CHECK_EQ(describe(TrimOp{ 1.0, 2.5 }), std::string("Trim to 1.00s - 2.50s"));
    CHECK_EQ(describe(CutOp{ 1.0, 2.0 }), std::string("Cut out 1.00s - 2.00s"));
    CHECK_EQ(describe(SpeedOp{ 1.5 }), std::string("Change speed to 1.5x"));
    CHECK_EQ(describe(ConvertToGifOp{}), std::string("Convert to GIF (15 fps, 480 px wide, 256 colors)"));
    CHECK_EQ(describe(OptimizeGifOp{}), std::string("Optimize GIF (128 colors, Bayer dither)"));
}
