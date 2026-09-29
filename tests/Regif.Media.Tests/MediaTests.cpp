// End-to-end checks of the FFmpeg backend against the small clips in tests/assets
// (generated with FFmpeg's testsrc2 pattern; see tests/assets/README.md).

#include "../Regif.Core.Tests/TestFramework.h"

#include <regif/FfmpegProcessor.h>
#include <regif/FrameGrabber.h>
#include <regif/Session.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <random>

namespace fs = std::filesystem;
using namespace regif;

namespace {

fs::path g_assets;

bool isNear(double actual, double expected, double tolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

#define CHECK_NEAR(actual, expected, tolerance)                                                      \
    do {                                                                                             \
        const double regifA = (actual);                                                              \
        if (!isNear(regifA, (expected), (tolerance)))                                                   \
            ::regif::test::reportFailure(__FILE__, __LINE__,                                         \
                "CHECK_NEAR(" #actual ") = " + std::to_string(regifA) + ", expected " #expected);   \
    } while (false)

class TempDir {
public:
    TempDir()
    {
        std::random_device device;
        m_path = fs::temp_directory_path() / ("regif-media-tests-" + std::to_string(device()));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

FfmpegProcessor& processor()
{
    static FfmpegProcessor instance;
    return instance;
}

} // namespace

TEST_CASE("probe a video")
{
    const MediaInfo info = processor().probe(g_assets / "clip.mp4");
    CHECK(info.kind == MediaKind::Video);
    CHECK_EQ(info.width, 160);
    CHECK_EQ(info.height, 90);
    CHECK_EQ(info.rotationDegrees, 0);
    CHECK_NEAR(info.durationSec, 2.0, 0.05);
    CHECK_NEAR(info.frameRate, 30.0, 0.01);
    CHECK_EQ(info.frameCount, std::int64_t(60));
}

TEST_CASE("probe a rotated phone video")
{
    // Tagged "rotate 90 degrees counter-clockwise" in the container.
    const MediaInfo info = processor().probe(g_assets / "rotated.mp4");
    CHECK_EQ(info.rotationDegrees, 270);
    CHECK_EQ(info.width, 90);
    CHECK_EQ(info.height, 160);
}

TEST_CASE("probe a GIF")
{
    const MediaInfo info = processor().probe(g_assets / "clip.gif");
    CHECK(info.kind == MediaKind::Gif);
    CHECK_EQ(info.width, 120);
    CHECK_EQ(info.height, 68);
    CHECK_EQ(info.frameCount, std::int64_t(30));
    CHECK_NEAR(info.durationSec, 2.0, 0.05);
    CHECK_NEAR(info.frameRate, 15.0, 0.5);
}

TEST_CASE("probing something that isn't media fails cleanly")
{
    TempDir temp;
    const fs::path bogus = temp.path() / "not-a-video.mp4";
    { std::ofstream(bogus) << "hello"; }
    CHECK_THROWS_AS(processor().probe(bogus), MediaError);
    CHECK_THROWS_AS(processor().probe(temp.path() / "missing.mp4"), MediaError);
}

TEST_CASE("video edits chain through lossless stages into a GIF")
{
    TempDir temp;
    Session session(g_assets / "clip.mp4", temp.path(), processor());

    std::vector<double> progress;
    session.apply(CropOp{ 3, 5, 81, 51 }, [&](double p) { progress.push_back(p); });
    CHECK_EQ(session.current().info.width, 81);
    CHECK_EQ(session.current().info.height, 51);
    CHECK(session.current().info.kind == MediaKind::Video);
    CHECK_NEAR(session.current().info.durationSec, 2.0, 0.1);
    CHECK(!progress.empty());

    session.apply(TrimOp{ 0.5, 1.5 });
    CHECK_NEAR(session.current().info.durationSec, 1.0, 0.1);

    session.apply(CutOp{ 0.25, 0.5 });
    CHECK_NEAR(session.current().info.durationSec, 0.75, 0.1);

    ConvertToGifOp convert;
    convert.fps = 10;
    convert.width = 64;
    session.apply(convert);
    const MediaInfo gif = session.current().info;
    CHECK(gif.kind == MediaKind::Gif);
    CHECK_EQ(gif.width, 64);
    CHECK_EQ(session.current().file.extension().string(), std::string(".gif"));
    CHECK_NEAR(gif.durationSec, 0.75, 0.15);
}

TEST_CASE("GIF speed, cut and optimize")
{
    TempDir temp;
    Session session(g_assets / "clip.gif", temp.path(), processor());

    session.apply(SpeedOp{ 2.0 });
    CHECK_NEAR(session.current().info.durationSec, 1.0, 0.1);
    CHECK_EQ(session.current().info.frameCount, std::int64_t(30)); // no frames dropped

    session.undo();
    session.apply(SpeedOp{ 0.5 });
    CHECK_NEAR(session.current().info.durationSec, 4.0, 0.2);
    CHECK_EQ(session.stages().size(), std::size_t(2)); // the 2x stage was replaced

    session.apply(CutOp{ 1.0, 2.0 });
    CHECK_NEAR(session.current().info.durationSec, 3.0, 0.2);

    const auto before = session.current().info.fileSizeBytes;
    OptimizeGifOp optimize;
    optimize.width = 60;
    optimize.gif.maxColors = 16;
    session.apply(optimize);
    CHECK_EQ(session.current().info.width, 60);
    CHECK(session.current().info.fileSizeBytes < before);

    session.apply(CropOp{ 0, 0, 40, 20 });
    CHECK_EQ(session.current().info.width, 40);
    CHECK_EQ(session.current().info.height, 20);
}

TEST_CASE("rotated videos come out upright")
{
    TempDir temp;
    Session session(g_assets / "rotated.mp4", temp.path(), processor());
    session.apply(CropOp{ 0, 0, 90, 150 });
    CHECK_EQ(session.current().info.width, 90);
    CHECK_EQ(session.current().info.height, 150);
    CHECK_EQ(session.current().info.rotationDegrees, 0);

    session.select(0);
    ConvertToGifOp convert;
    convert.width = 0;
    session.apply(convert);
    CHECK_EQ(session.current().info.width, 90);
    CHECK_EQ(session.current().info.height, 160);
}

TEST_CASE("cancelling a render leaves nothing behind")
{
    TempDir temp;
    Session session(g_assets / "clip.mp4", temp.path(), processor());
    CancellationToken token;
    token.cancel();
    CHECK_THROWS_AS(session.apply(ConvertToGifOp{}, {}, &token), OperationCancelled);
    CHECK_EQ(session.stages().size(), std::size_t(1));
    CHECK(fs::is_empty(session.workDirectory()));
}

TEST_CASE("export writes a playable copy")
{
    TempDir temp;
    Session session(g_assets / "clip.mp4", temp.path() / "work", processor());
    session.apply(ConvertToGifOp{});
    const fs::path out = temp.path() / "out.gif";
    session.exportCurrent(out);
    const MediaInfo info = processor().probe(out);
    CHECK(info.kind == MediaKind::Gif);
    CHECK_EQ(info.width, 480); // the default convert width, even when that scales up
}

TEST_CASE("frame grabber returns upright BGRA frames")
{
    FrameGrabber video(g_assets / "clip.mp4");
    const VideoFrameBgra a = video.grab(1.0);
    CHECK_EQ(a.width, 160);
    CHECK_EQ(a.height, 90);
    CHECK_EQ(a.pixels.size(), std::size_t(160 * 90 * 4));
    CHECK_NEAR(a.timeSec, 1.0, 0.05);
    const VideoFrameBgra b = video.grab(0.2); // seeking backwards works
    CHECK_NEAR(b.timeSec, 0.2, 0.05);
    const VideoFrameBgra end = video.grab(99.0); // past the end: last frame
    CHECK(end.timeSec > 1.9);

    FrameGrabber rotated(g_assets / "rotated.mp4");
    const VideoFrameBgra r = rotated.grab(0.5);
    CHECK_EQ(r.width, 90);
    CHECK_EQ(r.height, 160);

    FrameGrabber gif(g_assets / "clip.gif");
    const VideoFrameBgra g = gif.grab(1.0, 60);
    CHECK_EQ(g.width, 60);
    CHECK_EQ(g.height, 34);
    CHECK_NEAR(g.timeSec, 1.0, 0.1);
}

int main(int argc, char** argv)
{
    g_assets = argc > 1 ? fs::path(argv[1]) : fs::absolute(fs::path(argv[0])).parent_path() / "assets";
    if (!fs::exists(g_assets / "clip.mp4")) {
        std::cerr << "Test assets not found in " << g_assets.string() << "\n";
        return 2;
    }
    return regif::test::runAll();
}
