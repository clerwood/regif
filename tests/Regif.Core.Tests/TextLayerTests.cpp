#include "TestFramework.h"

#include <regif/Session.h>
#include <regif/TextLayer.h>

#include <cmath>

using namespace regif;

namespace {

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

MediaInfo video(int width = 1280, int height = 720, double duration = 10.0)
{
    MediaInfo info;
    info.kind = MediaKind::Video;
    info.width = width;
    info.height = height;
    info.durationSec = duration;
    info.frameRate = 30.0;
    return info;
}

MediaInfo gif(int width, int height, double duration)
{
    MediaInfo info = video(width, height, duration);
    info.kind = MediaKind::Gif;
    info.frameRate = 15.0;
    return info;
}

TextClip clip(double start, double end, double x = 100, double y = 50)
{
    TextClip c;
    c.id = 7;
    c.text = "Hello";
    c.startSec = start;
    c.endSec = end;
    c.x = x;
    c.y = y;
    c.style.fontFile = "C:/Windows/Fonts/segoeuib.ttf";
    c.style.fontSize = 40;
    return c;
}

// What FFmpeg's av_get_token does to an escaped string (we never emit quotes).
std::string unescapeOnce(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        out += s[i];
    }
    return out;
}

// True when every character the graph parser would act on is escaped.
bool safeForGraph(const std::string& s)
{
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (std::string_view("[],;:'= \n").find(s[i]) != std::string_view::npos) return false;
    }
    return true;
}

} // namespace

TEST_CASE("filter values survive both levels of FFmpeg's escaping")
{
    CHECK_EQ(escapeFilterValue("abc"), std::string("abc"));
    CHECK_EQ(escapeFilterValue("a:b"), std::string("a\\\\\\:b"));

    for (const std::string& text : { std::string("C:/Windows/Fonts/segoe ui.ttf"),
                                    std::string("it's 50% off: [now], or; later = never \\o/"),
                                    std::string("  two\nlines  "), std::string("caf\xC3\xA9 \xF0\x9F\x98\x80") }) {
        const std::string escaped = escapeFilterValue(text);
        CHECK(safeForGraph(escaped));
        CHECK_EQ(unescapeOnce(unescapeOnce(escaped)), text);
    }
}

TEST_CASE("drawtext filters carry the style and timing")
{
    TextClip c = clip(1.5, 3.0);
    c.style.stroke = { 0, 0, 0, 255 };
    c.style.strokeWidth = 2.6;
    const std::string f = drawTextFilter(c, "100", "50", true);
    CHECK_EQ(f, std::string("drawtext=fontfile=C\\\\\\:/Windows/Fonts/segoeuib.ttf:text=Hello:expansion=none:"
                            "fontsize=40:fontcolor=0xFFFFFFFF:text_align=C:y_align=font:x=100-text_w/2:y=50:"
                            "borderw=3:bordercolor=0x000000FF:enable=gte(t\\\\\\,1.5)*lt(t\\\\\\,3)"));

    c.style.align = TextAlign::Right;
    c.style.strokeWidth = 0;
    c.style.box = true;
    c.style.boxColor = { 16, 32, 48, 128 };
    c.style.boxPadding = 6;
    c.style.shadow = true;
    c.style.shadowColor = { 0, 0, 0, 100 };
    c.style.shadowX = -2;
    c.style.shadowY = 4;
    const std::string g = drawTextFilter(c, "10", "20", false);
    CHECK_CONTAINS(g, "text_align=R");
    CHECK_CONTAINS(g, "x=10-text_w:");
    CHECK_CONTAINS(g, ":box=1:boxcolor=0x10203080:boxborderw=6");
    CHECK_CONTAINS(g, ":shadowcolor=0x00000064:shadowx=-2:shadowy=4");
    CHECK(g.find("borderw=0") == std::string::npos);
    CHECK(g.find("bordercolor") == std::string::npos);
    CHECK(g.find("enable") == std::string::npos);
}

TEST_CASE("crops move text and resizes scale it")
{
    const TextClip c = clip(0, 2, 100, 50);
    const auto cropped = mapClipForward(c, CropOp{ 30, 20, 640, 360 }, video(), video(640, 360));
    CHECK(cropped && near(cropped->x, 70) && near(cropped->y, 30));

    const auto small = mapClipForward(c, ConvertToGifOp{}, video(), gif(480, 270, 10));
    CHECK(small && near(small->x, 37.5) && near(small->style.fontSize, 15) && near(small->style.strokeWidth, 1.125));
    CHECK(small && near(small->startSec, 0) && near(small->endSec, 2));

    const TextClip back = mapClipBackward(*small, ConvertToGifOp{}, video(), gif(480, 270, 10));
    CHECK(near(back.x, 100) && near(back.style.fontSize, 40));
}

TEST_CASE("trims, cuts and speed changes retime text")
{
    const MediaInfo v = video();

    const auto trimmed = mapClipForward(clip(1, 4), TrimOp{ 2, 8 }, v, video(1280, 720, 6));
    CHECK(trimmed && near(trimmed->startSec, 0) && near(trimmed->endSec, 2));
    CHECK(!mapClipForward(clip(8, 9), TrimOp{ 2, 8 }, v, video(1280, 720, 6)));

    // Cut 3-5: a clip across the cut shrinks, one inside disappears, one after moves up.
    const CutOp cut{ 3, 5 };
    const auto across = mapClipForward(clip(2, 6), cut, v, video(1280, 720, 8));
    CHECK(across && near(across->startSec, 2) && near(across->endSec, 4));
    CHECK(!mapClipForward(clip(3.5, 4.5), cut, v, video(1280, 720, 8)));
    const auto after = mapClipForward(clip(6, 7), cut, v, video(1280, 720, 8));
    CHECK(after && near(after->startSec, 4) && near(after->endSec, 5));

    // Back through the cut: starting at the join means after it, ending there means before it.
    const TextClip atJoin = mapClipBackward(clip(3, 4), cut, v, video(1280, 720, 8));
    CHECK(near(atJoin.startSec, 5) && near(atJoin.endSec, 6));
    const TextClip beforeJoin = mapClipBackward(clip(2, 3), cut, v, video(1280, 720, 8));
    CHECK(near(beforeJoin.startSec, 2) && near(beforeJoin.endSec, 3));

    // Cutting "to the end" with an end past the clip only removes what's there.
    const auto tail = mapClipForward(clip(9, 9.5), CutOp{ 8, 99 }, v, video(1280, 720, 8));
    CHECK(!tail);

    const MediaInfo g = gif(480, 270, 4);
    const auto fast = mapClipForward(clip(1, 3), SpeedOp{ 2 }, g, gif(480, 270, 2));
    CHECK(fast && near(fast->startSec, 0.5) && near(fast->endSec, 1.5));
    const TextClip slow = mapClipBackward(*fast, SpeedOp{ 2 }, g, gif(480, 270, 2));
    CHECK(near(slow.startSec, 1) && near(slow.endSec, 3));
}

TEST_CASE("burning in text re-encodes the stage with one drawtext per clip")
{
    TextLayer layer;
    layer.tracks.push_back({ 1, "Titles", { clip(0, 2) } });
    layer.tracks.push_back({ 2, "Subtitles", { clip(1, 3, 640, 600), clip(4, 5) } });
    layer.tracks[1].clips[1].text.clear(); // empty clips are skipped
    CHECK(!layer.empty());

    const RenderPlan video_ = planBurnIn(layer, video());
    CHECK_EQ(video_.encoder, std::string("ffv1"));
    CHECK(video_.filterGraph.starts_with("format=yuv444p,setpts=PTS-STARTPTS,drawtext="));
    std::size_t count = 0;
    for (std::size_t at = 0; (at = video_.filterGraph.find("drawtext=", at)) != std::string::npos; ++at) ++count;
    CHECK_EQ(count, std::size_t(2));
    CHECK_CONTAINS(video_.filterGraph, "x=640-text_w/2:y=600");

    const RenderPlan gif_ = planBurnIn(layer, gif(480, 270, 4));
    CHECK_EQ(gif_.encoder, std::string("gif"));
    CHECK(gif_.filterGraph.starts_with("format=bgra,setpts=PTS-STARTPTS,drawtext="));
    CHECK_CONTAINS(gif_.filterGraph, "paletteuse=dither=none");

    TextLayer blank;
    blank.tracks.push_back({ 1, "Empty", {} });
    CHECK(blank.empty());
}

TEST_CASE("quick captions spread words evenly and back to back")
{
    TextClip style = clip(0, 1, 320, 600);
    style.style.fontSize = 33;

    const auto one = makeCaptions("  the quick\nbrown   fox ", 1, 2.0, 6.0, style);
    CHECK_EQ(one.size(), std::size_t(4));
    CHECK_EQ(one[0].text, std::string("the"));
    CHECK_EQ(one[3].text, std::string("fox"));
    CHECK(near(one[0].startSec, 2.0) && near(one[0].endSec, 3.0));
    CHECK(near(one[3].startSec, 5.0) && near(one[3].endSec, 6.0));
    for (std::size_t i = 1; i < one.size(); ++i) CHECK(one[i].startSec == one[i - 1].endSec); // no gaps
    CHECK(near(one[2].x, 320) && near(one[2].style.fontSize, 33) && one[2].id == 0);

    const auto pairs = makeCaptions("a b c d e", 2, 0.0, 3.0, style);
    CHECK_EQ(pairs.size(), std::size_t(3));
    CHECK_EQ(pairs[0].text, std::string("a b"));
    CHECK_EQ(pairs[2].text, std::string("e"));
    CHECK(near(pairs[1].startSec, 1.0) && near(pairs[2].endSec, 3.0));

    CHECK(makeCaptions("   ", 1, 0.0, 3.0, style).empty());
    CHECK(makeCaptions("words", 1, 3.0, 3.0, style).empty());
    CHECK_EQ(makeCaptions("a b", 0, 0.0, 1.0, style).size(), std::size_t(2)); // at least one word each
}
