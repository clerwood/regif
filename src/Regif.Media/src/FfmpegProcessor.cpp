#include <regif/FfmpegProcessor.h>
#include <regif/Session.h> // pathToUtf8

#include "FfmpegCommon.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <new>

namespace regif {

using namespace ffmpeg;

namespace {

AVDictionary* toDictionary(const OptionList& options)
{
    AVDictionary* dict = nullptr;
    for (const auto& [key, value] : options) av_dict_set(&dict, key.c_str(), value.c_str(), 0);
    return dict;
}

// Throws if FFmpeg didn't recognise one of our options, which would otherwise be ignored silently.
void rejectUnusedOptions(AVDictionary* leftover, std::string_view where)
{
    if (const AVDictionaryEntry* entry = av_dict_get(leftover, "", nullptr, AV_DICT_IGNORE_SUFFIX))
        throw MediaError(std::format("The {} doesn't support the option '{}'.", where, entry->key));
}

// One render: input file -> decoder -> filter graph -> encoder -> output file.
class Renderer {
public:
    Renderer(const std::filesystem::path& input, const MediaInfo& inputInfo, const RenderPlan& plan,
             const std::filesystem::path& output, const ProgressCallback& progress, const CancellationToken& cancel)
        : m_input(openVideoInput(input, true))
        , m_inputInfo(inputInfo)
        , m_plan(plan)
        , m_outputUrl(pathToUtf8(output))
        , m_progress(progress)
        , m_cancel(cancel)
        , m_packet(makePacket())
        , m_decoded(makeFrame())
        , m_filtered(makeFrame())
        , m_encoded(makePacket())
    {
    }

    void run()
    {
        AVFormatContext* fmt = m_input.format.get();
        while (!m_filterFinished) {
            throwIfCancelled();
            const int read = av_read_frame(fmt, m_packet.get());
            if (read < 0) break; // end of file; a damaged tail simply ends the clip early
            if (m_packet->stream_index == m_input.streamIndex) decodePacket(m_packet.get());
            av_packet_unref(m_packet.get());
        }

        if (!m_filterFinished) decodePacket(nullptr); // flush the decoder
        if (!m_graph) throw MediaError("No frames could be decoded from this file.");

        if (!m_filterFinished) sendToFilter(nullptr);
        drainFilter();
        encode(nullptr); // flush the encoder
        if (m_framesEncoded == 0) throw MediaError("This change would leave no frames.");
        writeHeldPacket();

        check(av_write_trailer(m_output.get()), "Couldn't finish writing the file");
        check(avio_closep(&m_output->pb), "Couldn't finish writing the file");
    }

private:
    void throwIfCancelled() const
    {
        if (m_cancel.isCancelled()) throw OperationCancelled();
    }

    void decodePacket(const AVPacket* packet)
    {
        AVCodecContext* dec = m_input.decoder.get();
        const int sent = avcodec_send_packet(dec, packet);
        // Skip corrupt packets instead of failing the whole render.
        if (sent < 0 && sent != AVERROR_INVALIDDATA && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF)
            fail("Couldn't decode the video", sent);

        while (true) {
            const int received = avcodec_receive_frame(dec, m_decoded.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
            if (received == AVERROR_INVALIDDATA) continue;
            check(received, "Couldn't decode the video");

            throwIfCancelled();
            AVFrame* frame = m_decoded.get();
            frame->pts = framePts(frame);
            if (frame->pts == AV_NOPTS_VALUE) frame->pts = m_lastPts == AV_NOPTS_VALUE ? 0 : m_lastPts + 1;
            m_lastPts = frame->pts;

            if (!m_graph) initialize(frame);
            reportDecoded(frame->pts);

            sendToFilter(frame);
            av_frame_unref(frame);
            drainFilter();
            if (m_filterFinished) return;
        }
    }

    // The graph closes its input early when it needs no more frames (a trim that ends before
    // the clip does). FFmpeg reports that as AVERROR_EOF; the rest of the input is skipped.
    void sendToFilter(AVFrame* frame)
    {
        const int sent = av_buffersrc_add_frame_flags(m_source, frame, 0);
        if (sent == AVERROR_EOF) m_filterFinished = true;
        else check(sent, frame ? "Couldn't filter the video" : "Couldn't finish filtering");
    }

    void initialize(const AVFrame* first)
    {
        m_firstPts = first->pts;
        initializeFilters(first);
        initializeOutput();
    }

    void initializeFilters(const AVFrame* first)
    {
        m_graph.reset(avfilter_graph_alloc());
        if (!m_graph) throw std::bad_alloc();

        const AVRational tb = m_input.stream->time_base;
        const AVRational sar = first->sample_aspect_ratio.num > 0 ? first->sample_aspect_ratio : AVRational{ 1, 1 };
        const char* pixFmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(first->format));
        if (!pixFmt) throw MediaError("The video uses an unknown pixel format.");

        std::string args = std::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
                                       first->width, first->height, pixFmt, tb.num, tb.den, sar.num, sar.den);
        const AVRational rate = av_guess_frame_rate(m_input.format.get(), m_input.stream, nullptr);
        if (rate.num > 0 && rate.den > 0) args += std::format(":frame_rate={}/{}", rate.num, rate.den);

        check(avfilter_graph_create_filter(&m_source, avfilter_get_by_name("buffer"), "in", args.c_str(), nullptr,
                                           m_graph.get()),
              "Couldn't create the filter source");
        check(avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr,
                                           m_graph.get()),
              "Couldn't create the filter sink");

        // The plan's graph has one unlabeled input and output; connect them to "in" and "out".
        FilterInOutPtr outputs(avfilter_inout_alloc());
        FilterInOutPtr inputs(avfilter_inout_alloc());
        if (!outputs || !inputs) throw std::bad_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = m_source;
        outputs->pad_idx = 0;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = m_sink;
        inputs->pad_idx = 0;

        AVFilterInOut* in = inputs.release();
        AVFilterInOut* out = outputs.release();
        const int parsed = avfilter_graph_parse_ptr(m_graph.get(), m_plan.filterGraph.c_str(), &in, &out, nullptr);
        avfilter_inout_free(&in);
        avfilter_inout_free(&out);
        check(parsed, "Couldn't build the filter graph");
        check(avfilter_graph_config(m_graph.get(), nullptr), "Couldn't configure the filter graph");
    }

    void initializeOutput()
    {
        AVFormatContext* raw = nullptr;
        check(avformat_alloc_output_context2(&raw, nullptr, m_plan.container.c_str(), m_outputUrl.c_str()),
              "Couldn't create the output file");
        m_output.reset(raw);

        const AVCodec* codec = avcodec_find_encoder_by_name(m_plan.encoder.c_str());
        if (!codec) throw MediaError(std::format("This build of FFmpeg has no '{}' encoder.", m_plan.encoder));

        m_encoder.reset(avcodec_alloc_context3(codec));
        if (!m_encoder) throw std::bad_alloc();
        AVCodecContext* enc = m_encoder.get();
        enc->width = av_buffersink_get_w(m_sink);
        enc->height = av_buffersink_get_h(m_sink);
        enc->pix_fmt = static_cast<AVPixelFormat>(av_buffersink_get_format(m_sink));
        enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(m_sink);
        // Keep the filter's time base. Deriving it from the frame rate would merge frames whose
        // timestamps were compressed by a speed change.
        enc->time_base = av_buffersink_get_time_base(m_sink);
        const AVRational rate = av_buffersink_get_frame_rate(m_sink);
        if (rate.num > 0 && rate.den > 0) enc->framerate = rate;
        enc->thread_count = 0;
        if (m_output->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        std::unique_ptr<AVDictionary, DictionaryDeleter> encoderOptions(toDictionary(m_plan.encoderOptions));
        AVDictionary* opts = encoderOptions.release();
        const int opened = avcodec_open2(enc, codec, &opts);
        encoderOptions.reset(opts);
        check(opened, "Couldn't open the encoder");
        rejectUnusedOptions(encoderOptions.get(), "encoder");

        m_outStream = avformat_new_stream(m_output.get(), nullptr);
        if (!m_outStream) throw std::bad_alloc();
        check(avcodec_parameters_from_context(m_outStream->codecpar, enc), "Couldn't set up the output stream");
        m_outStream->time_base = enc->time_base;
        m_outStream->sample_aspect_ratio = enc->sample_aspect_ratio;

        if (!(m_output->oformat->flags & AVFMT_NOFILE)) {
            if (const int err = avio_open(&m_output->pb, m_outputUrl.c_str(), AVIO_FLAG_WRITE); err < 0)
                fail(std::format("Couldn't create {}", m_outputUrl), err);
        }

        std::unique_ptr<AVDictionary, DictionaryDeleter> muxerOptions(toDictionary(m_plan.muxerOptions));
        opts = muxerOptions.release();
        const int written = avformat_write_header(m_output.get(), &opts);
        muxerOptions.reset(opts);
        check(written, "Couldn't write the file header");
        rejectUnusedOptions(muxerOptions.get(), "output format");
    }

    void drainFilter()
    {
        while (true) {
            const int got = av_buffersink_get_frame(m_sink, m_filtered.get());
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return;
            check(got, "Couldn't filter the video");
            throwIfCancelled();
            m_filtered->pict_type = AV_PICTURE_TYPE_NONE;
            reportEncoded(m_filtered->pts);
            encode(m_filtered.get());
            av_frame_unref(m_filtered.get());
        }
    }

    void encode(const AVFrame* frame)
    {
        AVCodecContext* enc = m_encoder.get();
        check(avcodec_send_frame(enc, frame), "Couldn't encode the output");
        if (frame) ++m_framesEncoded;
        while (true) {
            const int got = avcodec_receive_packet(enc, m_encoded.get());
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return;
            check(got, "Couldn't encode the output");
            m_encoded->stream_index = m_outStream->index;
            av_packet_rescale_ts(m_encoded.get(), enc->time_base, m_outStream->time_base);
            queuePacket(m_encoded.get());
        }
    }

    // Packets are written one behind, so a packet without a duration can take it from the
    // next one's timestamp. Otherwise the last GIF frame would get the minimum delay (10 ms)
    // instead of its own, and flash past on every loop.
    void queuePacket(AVPacket* packet)
    {
        if (m_haveHeld) {
            if (packet->pts != AV_NOPTS_VALUE && m_held->pts != AV_NOPTS_VALUE && packet->pts > m_held->pts) {
                m_lastDelta = packet->pts - m_held->pts;
                if (m_held->duration <= 0) m_held->duration = m_lastDelta;
            }
            check(av_interleaved_write_frame(m_output.get(), m_held.get()), "Couldn't write the output");
        }
        av_packet_move_ref(m_held.get(), packet);
        m_haveHeld = true;
    }

    void writeHeldPacket()
    {
        if (!m_haveHeld) return;
        if (m_held->duration <= 0) m_held->duration = m_lastDelta; // the last frame lasts as long as the one before
        check(av_interleaved_write_frame(m_output.get(), m_held.get()), "Couldn't write the output");
        m_haveHeld = false;
    }

    // Progress is half decoding, half encoding. For GIFs the palette needs every frame
    // first, so the bar fills to about 50% while reading and the rest while encoding.
    void reportDecoded(std::int64_t pts)
    {
        if (m_inputInfo.durationSec <= 0.0) return;
        const double t = static_cast<double>(pts - m_firstPts) * av_q2d(m_input.stream->time_base);
        m_decodeFraction = std::clamp(t / m_inputInfo.durationSec, 0.0, 1.0);
        report();
    }

    void reportEncoded(std::int64_t pts)
    {
        if (m_plan.expectedDurationSec <= 0.0 || pts == AV_NOPTS_VALUE) return;
        const double t = static_cast<double>(pts) * av_q2d(av_buffersink_get_time_base(m_sink));
        m_encodeFraction = std::clamp(t / m_plan.expectedDurationSec, 0.0, 1.0);
        report();
    }

    void report()
    {
        if (!m_progress) return;
        const double value = std::min(0.99, 0.5 * m_decodeFraction + 0.5 * m_encodeFraction);
        if (value - m_lastReported >= 0.01) {
            m_lastReported = value;
            m_progress(value);
        }
    }

    VideoInput m_input;
    const MediaInfo& m_inputInfo;
    const RenderPlan& m_plan;
    std::string m_outputUrl;
    const ProgressCallback& m_progress;
    const CancellationToken& m_cancel;

    PacketPtr m_packet;
    FramePtr m_decoded;
    FramePtr m_filtered;
    PacketPtr m_encoded;
    PacketPtr m_held = makePacket(); // see queuePacket
    bool m_haveHeld = false;
    std::int64_t m_lastDelta = 0;

    FilterGraphPtr m_graph;
    AVFilterContext* m_source = nullptr; // owned by m_graph
    AVFilterContext* m_sink = nullptr;   // owned by m_graph
    OutputFormatPtr m_output;
    CodecContextPtr m_encoder;
    AVStream* m_outStream = nullptr;     // owned by m_output

    std::int64_t m_firstPts = 0;
    std::int64_t m_lastPts = AV_NOPTS_VALUE;
    std::int64_t m_framesEncoded = 0;
    bool m_filterFinished = false;
    double m_decodeFraction = 0.0;
    double m_encodeFraction = 0.0;
    double m_lastReported = 0.0;
};

} // namespace

FfmpegProcessor::FfmpegProcessor()
{
    av_log_set_level(AV_LOG_ERROR);
}

MediaInfo FfmpegProcessor::probe(const std::filesystem::path& file)
{
    VideoInput in = openVideoInput(file, false);
    const AVCodecParameters* par = in.stream->codecpar;
    const double tb = av_q2d(in.stream->time_base);

    MediaInfo info;
    info.kind = par->codec_id == AV_CODEC_ID_GIF ? MediaKind::Gif : MediaKind::Video;
    info.width = par->width;
    info.height = par->height;
    if (info.width <= 0 || info.height <= 0) throw MediaError("Couldn't determine the frame size of this file.");
    if (info.kind == MediaKind::Video) {
        info.rotationDegrees = displayRotation(in.stream);
        if (info.rotationDegrees == 90 || info.rotationDegrees == 270) std::swap(info.width, info.height);
    }

    if (info.kind == MediaKind::Gif) {
        // GIF headers don't store a duration or frame count, so read every packet (cheap: no decoding).
        PacketPtr packet = makePacket();
        std::int64_t first = AV_NOPTS_VALUE;
        std::int64_t end = AV_NOPTS_VALUE;
        while (av_read_frame(in.format.get(), packet.get()) >= 0) {
            if (packet->stream_index == in.streamIndex) {
                const std::int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
                if (ts != AV_NOPTS_VALUE) {
                    if (first == AV_NOPTS_VALUE || ts < first) first = ts;
                    end = std::max(end, ts + std::max<std::int64_t>(packet->duration, 0));
                }
                ++info.frameCount;
            }
            av_packet_unref(packet.get());
        }
        if (first != AV_NOPTS_VALUE && end > first) info.durationSec = static_cast<double>(end - first) * tb;
        if (info.durationSec > 0.0 && info.frameCount > 0)
            info.frameRate = static_cast<double>(info.frameCount) / info.durationSec;
    } else {
        if (in.stream->duration != AV_NOPTS_VALUE && in.stream->duration > 0)
            info.durationSec = static_cast<double>(in.stream->duration) * tb;
        else if (in.format->duration > 0)
            info.durationSec = static_cast<double>(in.format->duration) / AV_TIME_BASE;

        const AVRational rate = av_guess_frame_rate(in.format.get(), in.stream, nullptr);
        if (rate.num > 0 && rate.den > 0) info.frameRate = av_q2d(rate);

        info.frameCount = in.stream->nb_frames;
        if (info.frameCount <= 0 && info.durationSec > 0.0 && info.frameRate > 0.0)
            info.frameCount = std::llround(info.durationSec * info.frameRate);
    }
    return info;
}

void FfmpegProcessor::render(const std::filesystem::path& input, const MediaInfo& inputInfo, const RenderPlan& plan,
                             const std::filesystem::path& output, const ProgressCallback& progress,
                             const CancellationToken& cancel)
{
    Renderer(input, inputInfo, plan, output, progress, cancel).run();
}

} // namespace regif
