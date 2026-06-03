#include "video/video_file_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <utility>

extern "C" {
#if __has_include(<libavcodec/bsf.h>)
#include <libavcodec/bsf.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

VideoFileReader::VideoFileReader(std::string path) : path_(std::move(path)) {
    open();
}

VideoFileReader::~VideoFileReader() {
    close();
}

bool VideoFileReader::next_frame(EncodedVideoFrame &frame) {
    while (true) {
        AVPacket *filtered = av_packet_alloc();
        if (!filtered) {
            throw std::runtime_error("av_packet_alloc failed");
        }

        const int receive_result = av_bsf_receive_packet(bsf_context_, filtered);
        if (receive_result == 0) {
            frame.data.resize(static_cast<std::size_t>(filtered->size));
            std::copy(filtered->data, filtered->data + filtered->size,
                      reinterpret_cast<uint8_t *>(frame.data.data()));

            const int64_t duration = filtered->duration > 0
                                         ? av_rescale_q(filtered->duration,
                                                        format_context_->streams[video_stream_index_]
                                                            ->time_base,
                                                        AVRational{1, 90000})
                                         : fallback_duration_90khz_;
            frame.duration_90khz =
                static_cast<uint32_t>(std::max<int64_t>(1, duration));
            av_packet_free(&filtered);
            return true;
        }
        av_packet_free(&filtered);

        if (receive_result != AVERROR(EAGAIN) && receive_result != AVERROR_EOF) {
            throw std::runtime_error("av_bsf_receive_packet failed: " +
                                     ffmpeg_error(receive_result));
        }
        if (input_eof_) {
            return false;
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            throw std::runtime_error("av_packet_alloc failed");
        }

        int read_result = 0;
        do {
            read_result = av_read_frame(format_context_, packet);
        } while (read_result >= 0 && packet->stream_index != video_stream_index_ &&
                 (av_packet_unref(packet), true));

        if (read_result < 0) {
            input_eof_ = true;
            av_packet_free(&packet);
            av_bsf_send_packet(bsf_context_, nullptr);
            continue;
        }

        const int send_result = av_bsf_send_packet(bsf_context_, packet);
        av_packet_free(&packet);
        if (send_result < 0) {
            throw std::runtime_error("av_bsf_send_packet failed: " +
                                     ffmpeg_error(send_result));
        }
    }
}

void VideoFileReader::reset() {
    close();
    open();
}

void VideoFileReader::open() {
    int result = avformat_open_input(&format_context_, path_.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw std::runtime_error("cannot open video file '" + path_ + "': " +
                                 ffmpeg_error(result));
    }

    result = avformat_find_stream_info(format_context_, nullptr);
    if (result < 0) {
        throw std::runtime_error("cannot read video stream info: " + ffmpeg_error(result));
    }

    video_stream_index_ =
        av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        throw std::runtime_error("video file has no video stream");
    }

    AVStream *stream = format_context_->streams[video_stream_index_];
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264) {
        throw std::runtime_error("video stream must use H264 codec");
    }

    const AVRational fps = av_guess_frame_rate(format_context_, stream, nullptr);
    if (fps.num > 0 && fps.den > 0) {
        fallback_duration_90khz_ =
            static_cast<uint32_t>(std::max<int64_t>(
                1, av_rescale_q(1, av_inv_q(fps), AVRational{1, 90000})));
    }

    const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter) {
        throw std::runtime_error("FFmpeg h264_mp4toannexb bitstream filter not found");
    }

    result = av_bsf_alloc(filter, &bsf_context_);
    if (result < 0) {
        throw std::runtime_error("av_bsf_alloc failed: " + ffmpeg_error(result));
    }

    result = avcodec_parameters_copy(bsf_context_->par_in, stream->codecpar);
    if (result < 0) {
        throw std::runtime_error("avcodec_parameters_copy failed: " + ffmpeg_error(result));
    }
    bsf_context_->time_base_in = stream->time_base;

    result = av_bsf_init(bsf_context_);
    if (result < 0) {
        throw std::runtime_error("av_bsf_init failed: " + ffmpeg_error(result));
    }

    input_eof_ = false;
}

void VideoFileReader::close() {
    if (bsf_context_) {
        av_bsf_free(&bsf_context_);
    }
    if (format_context_) {
        avformat_close_input(&format_context_);
    }
    video_stream_index_ = -1;
    input_eof_ = false;
}
