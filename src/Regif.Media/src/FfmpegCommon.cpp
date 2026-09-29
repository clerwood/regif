#include "FfmpegCommon.h"

#include <regif/Session.h> // pathToUtf8

#include <cmath>
#include <format>
#include <new>

namespace regif::ffmpeg {

FramePtr makeFrame()
{
    FramePtr frame(av_frame_alloc());
    if (!frame) throw std::bad_alloc();
    return frame;
}

PacketPtr makePacket()
{
    PacketPtr packet(av_packet_alloc());
    if (!packet) throw std::bad_alloc();
    return packet;
}

std::string errorText(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(error, buffer, sizeof(buffer)) < 0) return std::format("error {}", error);
    return buffer;
}

void fail(std::string_view what, int error)
{
    throw MediaError(std::format("{} ({}).", what, errorText(error)));
}

VideoInput openVideoInput(const std::filesystem::path& file, bool openDecoder)
{
    VideoInput in;
    // FFmpeg's file protocol takes UTF-8 paths on Windows and opens them with shared read access.
    const std::string url = pathToUtf8(file);

    AVFormatContext* raw = nullptr;
    if (const int err = avformat_open_input(&raw, url.c_str(), nullptr, nullptr); err < 0)
        fail(std::format("Couldn't open {}", url), err);
    in.format.reset(raw);

    check(avformat_find_stream_info(raw, nullptr), "Couldn't read the file's stream information");

    const AVCodec* codec = nullptr;
    const int index = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0) throw MediaError("This file doesn't contain a video stream.");

    in.streamIndex = index;
    in.stream = raw->streams[index];
    for (unsigned i = 0; i < raw->nb_streams; ++i)
        if (static_cast<int>(i) != index) raw->streams[i]->discard = AVDISCARD_ALL;

    if (openDecoder) {
        if (!codec) throw MediaError("There's no decoder for this video format.");
        in.decoder.reset(avcodec_alloc_context3(codec));
        if (!in.decoder) throw std::bad_alloc();
        check(avcodec_parameters_to_context(in.decoder.get(), in.stream->codecpar), "Couldn't set up the decoder");
        in.decoder->pkt_timebase = in.stream->time_base;
        in.decoder->thread_count = 0; // automatic
        check(avcodec_open2(in.decoder.get(), codec, nullptr), "Couldn't open the decoder");
    }
    return in;
}

int displayRotation(const AVStream* stream)
{
    const AVCodecParameters* par = stream->codecpar;
    const AVPacketSideData* side = av_packet_side_data_get(par->coded_side_data, par->nb_coded_side_data,
                                                           AV_PKT_DATA_DISPLAYMATRIX);
    if (!side || side->size < 9 * sizeof(std::int32_t)) return 0;

    // av_display_rotation_get returns the counter-clockwise angle.
    const double ccw = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side->data));
    if (std::isnan(ccw)) return 0;
    int clockwise = static_cast<int>(std::lround(-ccw / 90.0)) * 90 % 360;
    if (clockwise < 0) clockwise += 360;
    return clockwise;
}

std::int64_t framePts(const AVFrame* frame)
{
    return frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
}

} // namespace regif::ffmpeg
