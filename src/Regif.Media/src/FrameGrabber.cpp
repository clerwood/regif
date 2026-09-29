#include <regif/FrameGrabber.h>

#include "FfmpegCommon.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace regif {

using namespace ffmpeg;

struct FrameGrabber::Impl {
    std::filesystem::path file;
    VideoInput input;
    bool isGif = false;
    int rotation = 0;
    PacketPtr packet = makePacket();
    FramePtr frame = makeFrame();
    FramePtr best = makeFrame();
    bool pending = false; // `frame` holds a decoded frame that decodeUntil read past its target
    SwsPtr sws;
    int swsSrcW = 0, swsSrcH = 0, swsSrcFormat = -1, swsDstW = 0, swsDstH = 0;

    // Decodes forward from the current position until the frame on screen at `target` is in `best`.
    // With `continuing`, carries on from the previous call (same position, ascending targets).
    bool decodeUntil(std::int64_t target, bool continuing = false)
    {
        AVCodecContext* dec = input.decoder.get();
        bool haveBest = continuing && best->buf[0] != nullptr;
        bool flushed = false;
        if (!continuing) {
            pending = false;
            av_frame_unref(best.get());
        }
        while (true) {
            int received = 0;
            if (pending) pending = false;
            else received = avcodec_receive_frame(dec, frame.get());
            if (received == AVERROR(EAGAIN) && !flushed) {
                const int read = av_read_frame(input.format.get(), packet.get());
                if (read < 0) {
                    avcodec_send_packet(dec, nullptr); // drain what's buffered
                    flushed = true;
                } else {
                    if (packet->stream_index == input.streamIndex) avcodec_send_packet(dec, packet.get());
                    av_packet_unref(packet.get());
                }
                continue;
            }
            if (received == AVERROR_INVALIDDATA) continue;
            if (received < 0) return haveBest; // EOF or error: show the last good frame

            const std::int64_t pts = framePts(frame.get());
            if (haveBest && pts != AV_NOPTS_VALUE && pts > target) {
                pending = true; // keep it for the next target
                return true;
            }
            av_frame_unref(best.get());
            av_frame_move_ref(best.get(), frame.get());
            haveBest = true;
            if (pts != AV_NOPTS_VALUE && pts >= target) return true;
        }
    }

    VideoFrameBgra convert(int maxDimension)
    {
        const AVFrame* src = best.get();
        const int longest = std::max(src->width, src->height);
        const double scale = longest > maxDimension && maxDimension > 0 ? double(maxDimension) / longest : 1.0;
        const int w = std::max(1, static_cast<int>(std::lround(src->width * scale)));
        const int h = std::max(1, static_cast<int>(std::lround(src->height * scale)));

        if (!sws || swsSrcW != src->width || swsSrcH != src->height || swsSrcFormat != src->format || swsDstW != w ||
            swsDstH != h) {
            sws.reset(sws_getContext(src->width, src->height, static_cast<AVPixelFormat>(src->format), w, h,
                                     AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr));
            if (!sws) throw MediaError("Couldn't convert this video's pixel format for the preview.");
            swsSrcW = src->width;
            swsSrcH = src->height;
            swsSrcFormat = src->format;
            swsDstW = w;
            swsDstH = h;
        }

        std::vector<std::uint8_t> scaled(static_cast<std::size_t>(w) * h * 4);
        std::uint8_t* dstData[4] = { scaled.data(), nullptr, nullptr, nullptr };
        int dstStride[4] = { w * 4, 0, 0, 0 };
        sws_scale(sws.get(), src->data, src->linesize, 0, src->height, dstData, dstStride);
        premultiply(scaled); // XAML bitmaps are premultiplied; video frames come out opaque, GIFs may not

        VideoFrameBgra out;
        const std::int64_t pts = framePts(src);
        const std::int64_t start = input.stream->start_time != AV_NOPTS_VALUE ? input.stream->start_time : 0;
        out.timeSec = pts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(pts - start) * av_q2d(input.stream->time_base);
        rotate(std::move(scaled), w, h, out);
        return out;
    }

    static void premultiply(std::vector<std::uint8_t>& bgra)
    {
        for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
            const unsigned a = bgra[i + 3];
            if (a == 0xFF) continue;
            for (std::size_t c = 0; c < 3; ++c) bgra[i + c] = static_cast<std::uint8_t>((bgra[i + c] * a + 127) / 255);
        }
    }

    void rotate(std::vector<std::uint8_t> pixels, int w, int h, VideoFrameBgra& out) const
    {
        if (rotation == 0) {
            out.width = w;
            out.height = h;
            out.pixels = std::move(pixels);
            return;
        }
        const bool quarter = rotation == 90 || rotation == 270;
        out.width = quarter ? h : w;
        out.height = quarter ? w : h;
        out.pixels.resize(pixels.size());
        const auto* src = reinterpret_cast<const std::uint32_t*>(pixels.data());
        auto* dst = reinterpret_cast<std::uint32_t*>(out.pixels.data());
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int dx = 0, dy = 0;
                switch (rotation) {
                case 90: dx = h - 1 - y; dy = x; break;           // clockwise
                case 180: dx = w - 1 - x; dy = h - 1 - y; break;
                case 270: dx = y; dy = w - 1 - x; break;          // counter-clockwise
                }
                dst[static_cast<std::size_t>(dy) * out.width + dx] = src[static_cast<std::size_t>(y) * w + x];
            }
        }
    }
};

void FrameGrabber::grabSequence(const std::vector<double>& seconds, int maxDimension, const FrameCallback& onFrame)
{
    Impl& impl = *m_impl;
    if (!impl.isGif) {
        // Seeking per frame is cheap for keyframe-rich stages and bounded by a GOP otherwise.
        for (std::size_t i = 0; i < seconds.size(); ++i) {
            std::optional<VideoFrameBgra> frame;
            try {
                frame = grab(seconds[i], maxDimension);
            } catch (const MediaError&) {
                continue;
            }
            if (!onFrame(i, std::move(*frame))) return;
        }
        return;
    }

    impl.input = openVideoInput(impl.file, true);
    AVStream* stream = impl.input.stream;
    const std::int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    for (std::size_t i = 0; i < seconds.size(); ++i) {
        const std::int64_t target =
            start + av_rescale_q(std::llround(std::max(0.0, seconds[i]) * AV_TIME_BASE), AV_TIME_BASE_Q,
                                 stream->time_base);
        if (!impl.decodeUntil(target, i > 0)) return; // past the end
        if (!onFrame(i, impl.convert(maxDimension))) return;
    }
}

FrameGrabber::FrameGrabber(const std::filesystem::path& file)
    : m_impl(std::make_unique<Impl>())
{
    m_impl->file = file;
    m_impl->input = openVideoInput(file, true);
    m_impl->isGif = m_impl->input.stream->codecpar->codec_id == AV_CODEC_ID_GIF;
    m_impl->rotation = displayRotation(m_impl->input.stream);
}

FrameGrabber::~FrameGrabber() = default;
FrameGrabber::FrameGrabber(FrameGrabber&&) noexcept = default;
FrameGrabber& FrameGrabber::operator=(FrameGrabber&&) noexcept = default;

VideoFrameBgra FrameGrabber::grab(double seconds, int maxDimension)
{
    Impl& impl = *m_impl;
    AVStream* stream = impl.input.stream;
    const std::int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const std::int64_t offset =
        av_rescale_q(std::llround(std::max(0.0, seconds) * AV_TIME_BASE), AV_TIME_BASE_Q, stream->time_base);
    const std::int64_t target = start + offset;

    if (impl.isGif) {
        // GIF frames build on the previous canvas and the demuxer can't seek reliably, so
        // decode from the start. GIFs are small enough for this to stay interactive.
        impl.input = openVideoInput(impl.file, true);
    } else {
        // Seek to the keyframe at or before the target, then decode forward.
        if (av_seek_frame(impl.input.format.get(), impl.input.streamIndex, target, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(impl.input.format.get(), impl.input.streamIndex, start, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(impl.input.decoder.get());
    }

    if (!impl.decodeUntil(target)) throw MediaError("Couldn't decode a frame at this position.");
    return impl.convert(maxDimension);
}

} // namespace regif
