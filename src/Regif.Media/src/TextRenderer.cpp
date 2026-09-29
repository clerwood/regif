#include <regif/TextRenderer.h>

#include "FfmpegCommon.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <new>

namespace regif {

using namespace ffmpeg;

namespace {

constexpr int kMaxCanvas = 8192;

struct Canvas {
    int width;
    int height;
    double anchorX;
    double anchorY;
};

// A transparent canvas that should hold the text, with the anchor placed so that every
// alignment grows into free space. Too small is detected afterwards and retried.
Canvas estimateCanvas(const TextClip& clip, double grow)
{
    int lines = 1;
    int longest = 0;
    int current = 0;
    for (const char c : clip.text) {
        if (c == '\n') {
            ++lines;
            longest = std::max(longest, current);
            current = 0;
        } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++current; // count code points, not UTF-8 bytes
        }
    }
    longest = std::max(longest, current);

    const TextStyle& s = clip.style;
    const double size = std::max(1.0, s.fontSize);
    const double margin = size + std::max(0.0, s.strokeWidth) + (s.box ? std::max(0.0, s.boxPadding) : 0.0) +
                          (s.shadow ? std::max(std::abs(s.shadowX), std::abs(s.shadowY)) : 0.0);
    const double textWidth = size * std::max(1, longest) * grow;
    const double textHeight = size * 1.5 * lines * grow;

    Canvas canvas{};
    canvas.width = std::clamp(static_cast<int>(std::ceil(textWidth + 2 * margin)), 16, kMaxCanvas);
    canvas.height = std::clamp(static_cast<int>(std::ceil(textHeight + 2 * margin)), 16, kMaxCanvas);
    switch (s.align) {
    case TextAlign::Left: canvas.anchorX = margin; break;
    case TextAlign::Center: canvas.anchorX = canvas.width / 2.0; break;
    case TextAlign::Right: canvas.anchorX = canvas.width - margin; break;
    }
    canvas.anchorY = margin;
    return canvas;
}

// Runs "color source -> drawtext -> sink" for one frame and returns it as RGBA.
FramePtr drawOnCanvas(const TextClip& clip, const Canvas& canvas)
{
    FilterGraphPtr graph(avfilter_graph_alloc());
    if (!graph) throw std::bad_alloc();

    AVFilterContext* sink = nullptr;
    check(avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get()),
          "Couldn't create the text renderer");

    const std::string description =
        std::format("color=c=0x00000000:s={}x{}:r=1:d=1,format=rgba,{}", canvas.width, canvas.height,
                    drawTextFilter(clip, std::format("{:.4f}", canvas.anchorX), std::format("{:.4f}", canvas.anchorY),
                                   false));

    // The description has no inputs and one unlabeled output, which connects to the sink.
    FilterInOutPtr inputs(avfilter_inout_alloc());
    if (!inputs) throw std::bad_alloc();
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink;
    inputs->pad_idx = 0;
    AVFilterInOut* in = inputs.release();
    AVFilterInOut* out = nullptr;
    const int parsed = avfilter_graph_parse_ptr(graph.get(), description.c_str(), &in, &out, nullptr);
    avfilter_inout_free(&in);
    avfilter_inout_free(&out);
    check(parsed, "Couldn't draw this text (check the font)");
    check(avfilter_graph_config(graph.get(), nullptr), "Couldn't draw this text (check the font)");

    FramePtr frame = makeFrame();
    check(av_buffersink_get_frame(sink, frame.get()), "Couldn't draw this text");
    if (frame->format != AV_PIX_FMT_RGBA) throw MediaError("The text renderer produced an unexpected format.");
    return frame;
}

struct Bounds {
    int left, top, right, bottom; // inclusive; right < left when empty
};

Bounds inkBounds(const AVFrame* frame)
{
    Bounds b{ frame->width, frame->height, -1, -1 };
    for (int y = 0; y < frame->height; ++y) {
        const std::uint8_t* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x) {
            if (row[x * 4 + 3] == 0) continue;
            b.left = std::min(b.left, x);
            b.right = std::max(b.right, x);
            b.top = std::min(b.top, y);
            b.bottom = std::max(b.bottom, y);
        }
    }
    return b;
}

} // namespace

RenderedText renderText(const TextClip& clip)
{
    RenderedText result;
    if (clip.text.empty()) return result;

    double grow = 1.0;
    for (int attempt = 0; attempt < 4; ++attempt, grow *= 2.0) {
        const Canvas canvas = estimateCanvas(clip, grow);
        const FramePtr frame = drawOnCanvas(clip, canvas);
        const Bounds b = inkBounds(frame.get());
        if (b.right < b.left) return result; // nothing visible (whitespace, or fully transparent colours)

        const bool touchesEdge = b.left == 0 || b.top == 0 || b.right == frame->width - 1 || b.bottom == frame->height - 1;
        const bool canGrow = canvas.width < kMaxCanvas || canvas.height < kMaxCanvas;
        if (touchesEdge && canGrow && attempt < 3) continue;

        // Crop and swap RGBA to BGRA. drawtext blends onto transparent black, which leaves
        // colours already premultiplied by alpha, as XAML bitmaps expect.
        VideoFrameBgra& image = result.image;
        image.width = b.right - b.left + 1;
        image.height = b.bottom - b.top + 1;
        image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4);
        for (int y = 0; y < image.height; ++y) {
            const std::uint8_t* src =
                frame->data[0] + static_cast<std::ptrdiff_t>(b.top + y) * frame->linesize[0] + b.left * 4;
            std::uint8_t* dst = image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4;
            for (int x = 0; x < image.width; ++x, src += 4, dst += 4) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        result.offsetX = b.left - canvas.anchorX;
        result.offsetY = b.top - canvas.anchorY;
        return result;
    }
    return result;
}

} // namespace regif
