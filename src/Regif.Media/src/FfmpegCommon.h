#pragma once

// Internal helpers shared by the FFmpeg-backed classes. Not part of the public API, so
// FFmpeg headers never leak into the app project.

#include <regif/MediaProcessor.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace regif::ffmpeg {

struct InputFormatDeleter {
    void operator()(AVFormatContext* c) const noexcept { avformat_close_input(&c); }
};
struct OutputFormatDeleter {
    void operator()(AVFormatContext* c) const noexcept
    {
        if (!c) return;
        if (c->pb && c->oformat && !(c->oformat->flags & AVFMT_NOFILE)) avio_closep(&c->pb);
        avformat_free_context(c);
    }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct FilterGraphDeleter {
    void operator()(AVFilterGraph* g) const noexcept { avfilter_graph_free(&g); }
};
struct FilterInOutDeleter {
    void operator()(AVFilterInOut* io) const noexcept { avfilter_inout_free(&io); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};
struct DictionaryDeleter {
    void operator()(AVDictionary* d) const noexcept { av_dict_free(&d); }
};

using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using FilterInOutPtr = std::unique_ptr<AVFilterInOut, FilterInOutDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

FramePtr makeFrame();
PacketPtr makePacket();

std::string errorText(int error);
[[noreturn]] void fail(std::string_view what, int error);
inline int check(int result, std::string_view what)
{
    if (result < 0) fail(what, result);
    return result;
}

// An opened input with only its best video stream enabled.
struct VideoInput {
    InputFormatPtr format;
    AVStream* stream = nullptr;
    int streamIndex = -1;
    CodecContextPtr decoder; // only when requested
};

VideoInput openVideoInput(const std::filesystem::path& file, bool openDecoder);

// Clockwise rotation a player applies to this stream (0, 90, 180 or 270), from the
// display matrix side data. Mirroring in the matrix is ignored.
int displayRotation(const AVStream* stream);

// Presentation timestamp of a decoded frame in stream time base, or AV_NOPTS_VALUE.
std::int64_t framePts(const AVFrame* frame);

} // namespace regif::ffmpeg
