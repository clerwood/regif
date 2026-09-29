#include <regif/TextLayer.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace regif {
namespace {

template <class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// Locale-independent decimal: 1.5 -> "1.5", 2.0 -> "2".
std::string num(double value)
{
    std::string s = std::format("{:.4f}", value);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

long whole(double value)
{
    return std::lround(std::max(0.0, value));
}

std::string color(const Rgba& c)
{
    return std::format("0x{:02X}{:02X}{:02X}{:02X}", c.r, c.g, c.b, c.a);
}

// The removed range of a cut, clamped to the clip so "cut to the end" works with any end.
struct CutRange {
    double start;
    double length;
};

CutRange cutRange(const CutOp& c, const MediaInfo& before)
{
    const double end = before.durationSec > 0.0 ? std::min(c.endSec, before.durationSec) : c.endSec;
    return { c.startSec, std::max(0.0, end - c.startSec) };
}

// after.width / before.width for operations that resize; 1 otherwise.
double resizeFactor(const MediaInfo& before, const MediaInfo& after)
{
    return before.width > 0 && after.width > 0 ? static_cast<double>(after.width) / before.width : 1.0;
}

void scaleClip(TextClip& clip, double k)
{
    clip.x *= k;
    clip.y *= k;
    TextStyle& s = clip.style;
    s.fontSize *= k;
    s.strokeWidth *= k;
    s.boxPadding *= k;
    s.shadowX *= k;
    s.shadowY *= k;
}

} // namespace

bool TextLayer::empty() const
{
    for (const TextTrack& track : tracks)
        for (const TextClip& clip : track.clips)
            if (!clip.text.empty() && clip.endSec > clip.startSec) return false;
    return true;
}

const TextClip* TextLayer::findClip(std::uint64_t clipId) const
{
    for (const TextTrack& track : tracks)
        for (const TextClip& clip : track.clips)
            if (clip.id == clipId) return &clip;
    return nullptr;
}

std::optional<TextClip> mapClipForward(const TextClip& clip, const Operation& op, const MediaInfo& before,
                                       const MediaInfo& after)
{
    TextClip out = clip;
    const bool kept = std::visit(Overloaded{
        [&](const CropOp& c) {
            out.x -= c.x;
            out.y -= c.y;
            return true;
        },
        [&](const TrimOp& t) {
            out.startSec = std::max(clip.startSec, t.startSec) - t.startSec;
            out.endSec = std::min(clip.endSec, t.endSec) - t.startSec;
            return out.endSec > out.startSec;
        },
        [&](const CutOp& c) {
            const CutRange cut = cutRange(c, before);
            auto map = [&](double t) {
                if (t < cut.start) return t;
                if (t < cut.start + cut.length) return cut.start; // inside the removed part
                return t - cut.length;
            };
            out.startSec = map(clip.startSec);
            out.endSec = map(clip.endSec);
            return out.endSec > out.startSec;
        },
        [&](const SpeedOp& s) {
            out.startSec /= s.factor;
            out.endSec /= s.factor;
            return true;
        },
        [&](const ConvertToGifOp&) {
            scaleClip(out, resizeFactor(before, after));
            return true;
        },
        [&](const OptimizeGifOp&) {
            scaleClip(out, resizeFactor(before, after));
            return true;
        },
    }, op);
    if (!kept) return std::nullopt;
    return out;
}

TextClip mapClipBackward(const TextClip& clip, const Operation& op, const MediaInfo& before, const MediaInfo& after)
{
    TextClip out = clip;
    std::visit(Overloaded{
        [&](const CropOp& c) {
            out.x += c.x;
            out.y += c.y;
        },
        [&](const TrimOp& t) {
            out.startSec += t.startSec;
            out.endSec += t.startSec;
        },
        [&](const CutOp& c) {
            // A clip starting at the join shows what came after the cut; one ending there, what came before.
            const CutRange cut = cutRange(c, before);
            if (clip.startSec >= cut.start) out.startSec += cut.length;
            if (clip.endSec > cut.start) out.endSec += cut.length;
        },
        [&](const SpeedOp& s) {
            out.startSec *= s.factor;
            out.endSec *= s.factor;
        },
        [&](const ConvertToGifOp&) { scaleClip(out, 1.0 / resizeFactor(before, after)); },
        [&](const OptimizeGifOp&) { scaleClip(out, 1.0 / resizeFactor(before, after)); },
    }, op);
    return out;
}

TextLayer mapLayerForward(const TextLayer& layer, const Operation& op, const MediaInfo& before, const MediaInfo& after)
{
    TextLayer out;
    out.tracks.reserve(layer.tracks.size());
    for (const TextTrack& track : layer.tracks) {
        TextTrack mapped{ track.id, track.name, {} };
        for (const TextClip& clip : track.clips)
            if (auto moved = mapClipForward(clip, op, before, after)) mapped.clips.push_back(std::move(*moved));
        out.tracks.push_back(std::move(mapped));
    }
    return out;
}

std::vector<TextClip> makeCaptions(std::string_view phrase, int wordsPerCaption, double startSec, double endSec,
                                   const TextClip& style)
{
    std::vector<std::string> words;
    std::string word;
    for (const char c : phrase) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!word.empty()) words.push_back(std::move(word));
            word.clear();
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(std::move(word));

    std::vector<TextClip> captions;
    if (words.empty() || !(endSec > startSec)) return captions;
    const std::size_t per = static_cast<std::size_t>(std::max(1, wordsPerCaption));
    const std::size_t count = (words.size() + per - 1) / per;
    const double slot = (endSec - startSec) / static_cast<double>(count);
    for (std::size_t i = 0; i < count; ++i) {
        TextClip clip = style;
        clip.id = 0;
        clip.text.clear();
        for (std::size_t w = i * per; w < std::min(words.size(), (i + 1) * per); ++w) {
            if (!clip.text.empty()) clip.text += ' ';
            clip.text += words[w];
        }
        clip.startSec = startSec + slot * static_cast<double>(i);
        clip.endSec = i + 1 == count ? endSec : startSec + slot * static_cast<double>(i + 1); // no gap from rounding
        captions.push_back(std::move(clip));
    }
    return captions;
}

std::string escapeFilterValue(std::string_view value)
{
    // Backslash-escaping any character is harmless to av_get_token, so one set serves both
    // levels: the option parser splits on ':' and the graph parser on "[],;"; both strip
    // unescaped leading and trailing whitespace and treat quotes and backslashes specially.
    auto escape = [](std::string_view in) {
        std::string out;
        out.reserve(in.size() * 2);
        for (const char c : in) {
            switch (c) {
            case '\\': case '\'': case ':': case '=': case '[': case ']': case ',': case ';':
            case ' ': case '\t': case '\n': case '\r':
                out += '\\';
                break;
            default:
                break;
            }
            out += c;
        }
        return out;
    };
    return escape(escape(value));
}

std::string drawTextFilter(const TextClip& clip, std::string_view anchorX, std::string_view anchorY, bool timed)
{
    const TextStyle& s = clip.style;
    std::string x;
    switch (s.align) {
    case TextAlign::Left: x = std::string(anchorX); break;
    case TextAlign::Center: x = std::format("{}-text_w/2", anchorX); break;
    case TextAlign::Right: x = std::format("{}-text_w", anchorX); break;
    }
    const char* align = s.align == TextAlign::Left ? "L" : s.align == TextAlign::Right ? "R" : "C";

    std::string f = "drawtext=";
    if (!s.fontFile.empty()) f += std::format("fontfile={}:", escapeFilterValue(s.fontFile));
    f += std::format("text={}:expansion=none:fontsize={}:fontcolor={}:text_align={}:y_align=font:x={}:y={}",
                     escapeFilterValue(clip.text), num(std::max(1.0, s.fontSize)), color(s.fill), align,
                     escapeFilterValue(x), escapeFilterValue(anchorY));
    if (whole(s.strokeWidth) > 0 && s.stroke.a > 0)
        f += std::format(":borderw={}:bordercolor={}", whole(s.strokeWidth), color(s.stroke));
    if (s.box) f += std::format(":box=1:boxcolor={}:boxborderw={}", color(s.boxColor), whole(s.boxPadding));
    if (s.shadow) {
        f += std::format(":shadowcolor={}:shadowx={}:shadowy={}", color(s.shadowColor), std::lround(s.shadowX),
                         std::lround(s.shadowY));
    }
    if (timed) {
        f += std::format(":enable={}",
                         escapeFilterValue(std::format("gte(t,{})*lt(t,{})", num(clip.startSec), num(clip.endSec))));
    }
    return f;
}

RenderPlan planBurnIn(const TextLayer& layer, const MediaInfo& stage)
{
    // Timestamps from zero, so clip times match what the timeline shows.
    std::string body = "setpts=PTS-STARTPTS";
    for (const TextTrack& track : layer.tracks) {
        for (const TextClip& clip : track.clips) {
            if (clip.text.empty() || clip.endSec <= clip.startSec) continue;
            body += ',';
            body += drawTextFilter(clip, num(clip.x), num(clip.y), true);
        }
    }
    return planReencode(stage, body);
}

} // namespace regif
