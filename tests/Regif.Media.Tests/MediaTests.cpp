// End-to-end checks of the FFmpeg backend against the small clips in tests/assets
// (generated with FFmpeg's testsrc2 pattern; see tests/assets/README.md).

#include "../Regif.Core.Tests/TestFramework.h"

#include <regif/FfmpegProcessor.h>
#include <regif/FrameGrabber.h>
#include <regif/Session.h>
#include <regif/TextRenderer.h>

#include <cmath>
#include <cstdlib>
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

TEST_CASE("stepping forward like playback matches fresh grabs")
{
    for (const char* name : { "clip.mp4", "clip.gif" }) {
        FrameGrabber playing(g_assets / name);
        int mismatches = 0;
        double last = -1.0;
        // 40 steps a second: faster than either clip's frame rate, so some steps repeat a frame.
        for (double t = 0.0; t < 2.0; t += 0.025) {
            const VideoFrameBgra a = playing.grab(t, 80);
            const VideoFrameBgra b = FrameGrabber(g_assets / name).grab(t, 80);
            if (std::abs(a.timeSec - b.timeSec) > 1e-6 || a.pixels != b.pixels) ++mismatches;
            CHECK(a.timeSec >= last); // never goes backwards
            last = a.timeSec;
        }
        CHECK_EQ(mismatches, 0);

        // Jumps back and far ahead still land on the right frame.
        CHECK_NEAR(playing.grab(0.2, 80).timeSec, FrameGrabber(g_assets / name).grab(0.2, 80).timeSec, 1e-6);
        CHECK_NEAR(playing.grab(1.9, 80).timeSec, FrameGrabber(g_assets / name).grab(1.9, 80).timeSec, 1e-6);
    }
}

TEST_CASE("frame grabber sequences match single grabs")
{
    // Two targets inside one GIF frame (15 fps) check that the one-pass decode doesn't skip ahead.
    const std::vector<double> times{ 0.1, 0.12, 0.5, 1.0, 1.9 };
    for (const char* name : { "clip.gif", "clip.mp4" }) {
        FrameGrabber sequence(g_assets / name);
        FrameGrabber single(g_assets / name);
        std::vector<std::size_t> seen;
        sequence.grabSequence(times, 60, [&](std::size_t index, VideoFrameBgra&& frame) {
            seen.push_back(index);
            const VideoFrameBgra expected = single.grab(times[index], 60);
            CHECK_NEAR(frame.timeSec, expected.timeSec, 1e-6);
            CHECK_EQ(frame.width, expected.width);
            CHECK(frame.pixels == expected.pixels);
            return true;
        });
        CHECK_EQ(seen.size(), times.size());

        std::size_t calls = 0;
        sequence.grabSequence(times, 60, [&](std::size_t, VideoFrameBgra&&) { return ++calls < 2; });
        CHECK_EQ(calls, std::size_t(2)); // stops when the callback returns false
    }
}

namespace {

std::string windowsFont(const char* file)
{
    const char* windir = std::getenv("WINDIR");
    return pathToUtf8(fs::path(windir ? windir : "C:\\Windows") / "Fonts" / file);
}

TextClip sampleText(TextAlign align)
{
    TextClip clip;
    clip.text = "Hi";
    clip.startSec = 0.0;
    clip.endSec = 2.0;
    clip.x = 80;
    clip.y = 20;
    clip.style.fontFile = windowsFont("arialbd.ttf");
    clip.style.fontSize = 30;
    clip.style.align = align;
    clip.style.strokeWidth = 2;
    return clip;
}

} // namespace

TEST_CASE("text renders around its anchor for each alignment")
{
    const RenderedText center = renderText(sampleText(TextAlign::Center));
    CHECK(center.image.width > 20 && center.image.height > 15);
    CHECK_EQ(center.image.pixels.size(), std::size_t(center.image.width) * center.image.height * 4);
    CHECK_NEAR(center.offsetX + center.image.width / 2.0, 0.0, 3.0);
    CHECK(center.offsetY > -4.0 && center.offsetY < 12.0); // y is the top of the font's line box

    const RenderedText left = renderText(sampleText(TextAlign::Left));
    CHECK_NEAR(left.offsetX, 0.0, 4.0);
    const RenderedText right = renderText(sampleText(TextAlign::Right));
    CHECK_NEAR(right.offsetX + right.image.width, 0.0, 4.0);

    // Premultiplied: no colour channel exceeds alpha.
    bool premultiplied = true;
    for (std::size_t i = 0; i + 3 < center.image.pixels.size(); i += 4)
        for (std::size_t c = 0; c < 3; ++c) premultiplied &= center.image.pixels[i + c] <= center.image.pixels[i + 3];
    CHECK(premultiplied);

    TextClip blank = sampleText(TextAlign::Center);
    blank.text.clear();
    CHECK(renderText(blank).image.pixels.empty());

    TextClip badFont = sampleText(TextAlign::Center);
    badFont.style.fontFile = pathToUtf8(g_assets / "no-such-font.ttf");
    CHECK_THROWS_AS(renderText(badFont), MediaError);
}

// Bounding box of the pixels that clearly differ between two same-sized frames.
struct DiffBox {
    int left, top, right, bottom;
    bool empty() const { return right < left; }
};

static DiffBox diffBox(const VideoFrameBgra& a, const VideoFrameBgra& b)
{
    DiffBox box{ a.width, a.height, -1, -1 };
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * a.width + x) * 4;
            int diff = 0;
            for (std::size_t c = 0; c < 3; ++c) diff = std::max(diff, std::abs(int(a.pixels[i + c]) - int(b.pixels[i + c])));
            if (diff < 80) continue;
            box.left = std::min(box.left, x);
            box.right = std::max(box.right, x);
            box.top = std::min(box.top, y);
            box.bottom = std::max(box.bottom, y);
        }
    }
    return box;
}

TEST_CASE("exported text lands where and when the preview draws it")
{
    // Two exports through the same pipeline, with the text shown in the first or second second.
    // Comparing them isolates the text from colour conversion differences.
    TempDir temp;
    const TextClip clip = sampleText(TextAlign::Center);
    auto exportWith = [&](double start, double end, const char* name) {
        Session session(g_assets / "clip.mp4", temp.path(), processor());
        TextClip timed = clip;
        timed.startSec = start;
        timed.endSec = end;
        session.addTextClip(session.addTextTrack("Titles"), timed);
        CHECK_EQ(session.exportExtension(), std::string(".mkv"));
        const fs::path out = temp.path() / name;
        session.exportCurrent(out);
        return out;
    };
    const fs::path first = exportWith(0.0, 1.0, "first.mkv");
    const fs::path second = exportWith(1.0, 2.0, "second.mkv");

    const RenderedText preview = renderText(clip);
    const double expectLeft = clip.x + preview.offsetX, expectTop = clip.y + preview.offsetY;
    for (const double t : { 0.5, 1.5 }) {
        const VideoFrameBgra a = FrameGrabber(first).grab(t);
        const VideoFrameBgra b = FrameGrabber(second).grab(t);
        CHECK_EQ(a.width, 160);
        const DiffBox box = diffBox(a, b);
        CHECK(!box.empty());
        CHECK(box.left >= expectLeft - 1 && box.top >= expectTop - 1);
        CHECK(box.right <= expectLeft + preview.image.width + 1 && box.bottom <= expectTop + preview.image.height + 1);
        CHECK(box.right - box.left + 1 >= preview.image.width * 0.7);
        CHECK(box.bottom - box.top + 1 >= preview.image.height * 0.6);
    }
}

TEST_CASE("text burned into a GIF keeps every frame")
{
    TempDir temp;
    Session session(g_assets / "clip.gif", temp.path(), processor());
    session.addTextClip(session.addTextTrack("Titles"), sampleText(TextAlign::Center));
    CHECK_EQ(session.exportExtension(), std::string(".gif"));
    const fs::path out = temp.path() / "titled.gif";
    session.exportCurrent(out);
    const MediaInfo before = processor().probe(g_assets / "clip.gif");
    const MediaInfo after = processor().probe(out);
    CHECK_EQ(after.frameCount, before.frameCount);
    // The last frame keeps a full delay (the source ends 70 ms after it; we give it the previous 60 ms).
    CHECK_NEAR(after.durationSec, before.durationSec, 0.02);
    CHECK_EQ(after.width, before.width);
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
