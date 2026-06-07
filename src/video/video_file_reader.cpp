#include "video/video_file_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void check_ffmpeg(int result, const char *operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 ffmpeg_error(result));
    }
}

} // namespace

VideoFileReader::VideoFileReader(std::string path) : path_(std::move(path)) {
    open();
}

VideoFileReader::~VideoFileReader() {
    close();
}

bool VideoFileReader::next_frame(EncodedVideoFrame &frame,
                                 const VideoProfile &profile,
                                 bool force_keyframe) {
    if (!encoder_configured_ || encoder_profile_ != profile) {
        configure_encoder(profile);
        force_keyframe = true;
    }

    while (next_decoded_frame()) {
        if (!should_output_decoded_frame(profile.fps)) {
            av_frame_unref(decoded_frame_);
            continue;
        }

        sws_context_ = sws_getCachedContext(
            sws_context_,
            decoded_frame_->width,
            decoded_frame_->height,
            static_cast<AVPixelFormat>(decoded_frame_->format),
            profile.width,
            profile.height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        if (!sws_context_) {
            throw std::runtime_error("sws_getCachedContext failed");
        }

        check_ffmpeg(av_frame_make_writable(scaled_frame_),
                     "av_frame_make_writable");
        sws_scale(sws_context_,
                  decoded_frame_->data,
                  decoded_frame_->linesize,
                  0,
                  decoded_frame_->height,
                  scaled_frame_->data,
                  scaled_frame_->linesize);
        av_frame_unref(decoded_frame_);

        scaled_frame_->pts = encoder_pts_++;
        scaled_frame_->pict_type =
            force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        scaled_frame_->key_frame = force_keyframe ? 1 : 0;

        check_ffmpeg(avcodec_send_frame(encoder_context_, scaled_frame_),
                     "avcodec_send_frame");

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            throw std::runtime_error("av_packet_alloc failed");
        }
        const int receive_result =
            avcodec_receive_packet(encoder_context_, packet);
        if (receive_result == AVERROR(EAGAIN)) {
            av_packet_free(&packet);
            continue;
        }
        if (receive_result < 0) {
            const std::string error = ffmpeg_error(receive_result);
            av_packet_free(&packet);
            throw std::runtime_error("avcodec_receive_packet failed: " + error);
        }

        frame.data.resize(static_cast<std::size_t>(packet->size));
        std::memcpy(frame.data.data(), packet->data,
                    static_cast<std::size_t>(packet->size));
        frame.duration_90khz =
            static_cast<uint32_t>(std::max(1, 90000 / profile.fps));
        frame.epoch = epoch_;
        frame.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_free(&packet);
        return true;
    }
    return false;
}

void VideoFileReader::reset() {
    close();
    open();
}

void VideoFileReader::open() {
    int result =
        avformat_open_input(&format_context_, path_.c_str(), nullptr, nullptr);
    if (result < 0) {
        throw std::runtime_error("cannot open video file '" + path_ + "': " +
                                 ffmpeg_error(result));
    }

    check_ffmpeg(avformat_find_stream_info(format_context_, nullptr),
                 "avformat_find_stream_info");

    video_stream_index_ =
        av_find_best_stream(format_context_, AVMEDIA_TYPE_VIDEO,
                            -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        throw std::runtime_error("video file has no video stream");
    }

    AVStream *stream = format_context_->streams[video_stream_index_];
    stream_time_base_num_ = stream->time_base.num;
    stream_time_base_den_ = stream->time_base.den;

    const AVCodec *decoder =
        avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        throw std::runtime_error("input video decoder not found");
    }
    decoder_context_ = avcodec_alloc_context3(decoder);
    if (!decoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 decoder failed");
    }
    check_ffmpeg(avcodec_parameters_to_context(
                     decoder_context_, stream->codecpar),
                 "avcodec_parameters_to_context");
    decoder_context_->thread_count = 2;
    check_ffmpeg(avcodec_open2(decoder_context_, decoder, nullptr),
                 "avcodec_open2 decoder");

    decoded_frame_ = av_frame_alloc();
    if (!decoded_frame_) {
        throw std::runtime_error("av_frame_alloc decoder failed");
    }

    input_eof_ = false;
    decoder_flushed_ = false;
    next_output_source_seconds_ = -1.0;
}

void VideoFileReader::close() {
    close_encoder();
    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
    av_frame_free(&decoded_frame_);
    avcodec_free_context(&decoder_context_);
    if (format_context_) {
        avformat_close_input(&format_context_);
    }
    video_stream_index_ = -1;
    input_eof_ = false;
    decoder_flushed_ = false;
    next_output_source_seconds_ = -1.0;
}

void VideoFileReader::close_encoder() {
    av_frame_free(&scaled_frame_);
    avcodec_free_context(&encoder_context_);
    encoder_configured_ = false;
}

void VideoFileReader::configure_encoder(const VideoProfile &profile) {
    close_encoder();

    const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        throw std::runtime_error("FFmpeg libx264 encoder not found");
    }

    encoder_context_ = avcodec_alloc_context3(encoder);
    if (!encoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 encoder failed");
    }

    encoder_context_->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder_context_->codec_id = AV_CODEC_ID_H264;
    encoder_context_->width = profile.width;
    encoder_context_->height = profile.height;
    encoder_context_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_context_->time_base = AVRational{1, profile.fps};
    encoder_context_->framerate = AVRational{profile.fps, 1};
    encoder_context_->bit_rate =
        static_cast<int64_t>(profile.bitrate_kbps) * 950;
    encoder_context_->rc_max_rate =
        static_cast<int64_t>(profile.bitrate_kbps) * 1000;
    encoder_context_->rc_buffer_size =
        static_cast<int>(encoder_context_->bit_rate / 2);
    encoder_context_->gop_size = profile.fps;
    encoder_context_->max_b_frames = 0;
    encoder_context_->refs = 1;
    encoder_context_->thread_count = 2;

    av_opt_set(encoder_context_->priv_data, "preset", "veryfast", 0);
    av_opt_set(encoder_context_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(encoder_context_->priv_data, "profile", "baseline", 0);

    const std::string x264_params =
        "repeat-headers=1:scenecut=0:keyint=" +
        std::to_string(profile.fps) +
        ":min-keyint=" + std::to_string(profile.fps) +
        ":bframes=0:ref=1:force-cfr=1";
    av_opt_set(encoder_context_->priv_data, "x264-params",
               x264_params.c_str(), 0);

    check_ffmpeg(avcodec_open2(encoder_context_, encoder, nullptr),
                 "avcodec_open2 encoder");

    scaled_frame_ = av_frame_alloc();
    if (!scaled_frame_) {
        throw std::runtime_error("av_frame_alloc encoder failed");
    }
    scaled_frame_->format = AV_PIX_FMT_YUV420P;
    scaled_frame_->width = profile.width;
    scaled_frame_->height = profile.height;
    check_ffmpeg(av_frame_get_buffer(scaled_frame_, 32),
                 "av_frame_get_buffer");

    encoder_profile_ = profile;
    encoder_configured_ = true;
    encoder_pts_ = 0;
    next_output_source_seconds_ = -1.0;
    ++epoch_;
    if (epoch_ == 0) {
        epoch_ = 1;
    }
}

bool VideoFileReader::next_decoded_frame() {
    while (true) {
        av_frame_unref(decoded_frame_);
        const int receive_result =
            avcodec_receive_frame(decoder_context_, decoded_frame_);
        if (receive_result == 0) {
            return true;
        }
        if (receive_result == AVERROR_EOF) {
            return false;
        }
        if (receive_result != AVERROR(EAGAIN)) {
            throw std::runtime_error("avcodec_receive_frame failed: " +
                                     ffmpeg_error(receive_result));
        }

        if (input_eof_) {
            if (!decoder_flushed_) {
                check_ffmpeg(avcodec_send_packet(decoder_context_, nullptr),
                             "avcodec_send_packet flush");
                decoder_flushed_ = true;
                continue;
            }
            return false;
        }

        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            throw std::runtime_error("av_packet_alloc failed");
        }
        int read_result = 0;
        do {
            read_result = av_read_frame(format_context_, packet);
        } while (read_result >= 0 &&
                 packet->stream_index != video_stream_index_ &&
                 (av_packet_unref(packet), true));

        if (read_result < 0) {
            input_eof_ = true;
            av_packet_free(&packet);
            continue;
        }

        const int send_result =
            avcodec_send_packet(decoder_context_, packet);
        av_packet_free(&packet);
        if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
            throw std::runtime_error("avcodec_send_packet failed: " +
                                     ffmpeg_error(send_result));
        }
    }
}

bool VideoFileReader::should_output_decoded_frame(int fps) {
    double source_seconds = 0.0;
    if (decoded_frame_->best_effort_timestamp != AV_NOPTS_VALUE &&
        stream_time_base_den_ > 0) {
        source_seconds =
            static_cast<double>(decoded_frame_->best_effort_timestamp) *
            static_cast<double>(stream_time_base_num_) /
            static_cast<double>(stream_time_base_den_);
    } else if (next_output_source_seconds_ >= 0.0) {
        source_seconds = next_output_source_seconds_;
    }

    if (next_output_source_seconds_ < 0.0) {
        next_output_source_seconds_ = source_seconds;
    }
    if (source_seconds + 0.0001 < next_output_source_seconds_) {
        return false;
    }
    next_output_source_seconds_ += 1.0 / static_cast<double>(fps);
    return true;
}
