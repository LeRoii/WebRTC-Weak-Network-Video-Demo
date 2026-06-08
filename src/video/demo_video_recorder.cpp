#include "video/demo_video_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
constexpr int kOutputWidth = 1280;
constexpr int kOutputHeight = 720;
constexpr int kOutputFps = 30;
constexpr int64_t kOutputBitrate = 4'000'000;

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
}

DemoVideoRecorder::DemoVideoRecorder() = default;

DemoVideoRecorder::~DemoVideoRecorder() {
    stop();
}

void DemoVideoRecorder::start(std::string path) {
    stop();
    path_ = std::move(path);
    stopping_.store(false);
    active_.store(true);
    thread_ = std::thread([this] { record_loop(); });
}

void DemoVideoRecorder::stop() {
    stopping_.store(true);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        av_frame_free(&latest_source_);
        source_version_ = 0;
    }
    active_.store(false);
}

void DemoVideoRecorder::submit(const AVFrame *frame) {
    if (!active_.load() || !frame) {
        return;
    }

    AVFrame *copy = av_frame_clone(frame);
    if (!copy) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        av_frame_free(&latest_source_);
        latest_source_ = copy;
        ++source_version_;
    }
    cv_.notify_one();
}

bool DemoVideoRecorder::active() const {
    return active_.load();
}

void DemoVideoRecorder::record_loop() {
    try {
        uint64_t rendered_version = 0;
        AVFrame *source = nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_.load() || latest_source_ != nullptr;
            });
            if (stopping_.load() || !latest_source_) {
                return;
            }
            source = av_frame_clone(latest_source_);
            rendered_version = source_version_;
        }

        open_output();
        update_canvas(source);
        av_frame_free(&source);

        const auto frame_interval =
            std::chrono::microseconds(1'000'000 / kOutputFps);
        auto next_frame_at = std::chrono::steady_clock::now();

        while (!stopping_.load()) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_until(lock, next_frame_at, [this, rendered_version] {
                    return stopping_.load() ||
                           source_version_ != rendered_version;
                });
                if (source_version_ != rendered_version && latest_source_) {
                    source = av_frame_clone(latest_source_);
                    rendered_version = source_version_;
                }
            }

            if (source) {
                update_canvas(source);
                av_frame_free(&source);
            }
            if (stopping_.load()) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now < next_frame_at) {
                continue;
            }
            encode_canvas();
            next_frame_at += frame_interval;
        }

        close_output();
        std::cerr << "demo_recording_complete=" << path_
                  << " frames=" << next_pts_ << std::endl;
    } catch (const std::exception &error) {
        std::cerr << "demo_recording_failed=" << error.what() << std::endl;
        close_output();
    }
    active_.store(false);
}

void DemoVideoRecorder::open_output() {
    check_ffmpeg(
        avformat_alloc_output_context2(
            &format_context_, nullptr, nullptr, path_.c_str()),
        "avformat_alloc_output_context2");
    if (!format_context_) {
        throw std::runtime_error("cannot determine demo output format");
    }

    const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        throw std::runtime_error("FFmpeg libx264 encoder not found");
    }

    AVStream *stream = avformat_new_stream(format_context_, nullptr);
    if (!stream) {
        throw std::runtime_error("avformat_new_stream failed");
    }
    stream_index_ = stream->index;

    encoder_context_ = avcodec_alloc_context3(encoder);
    if (!encoder_context_) {
        throw std::runtime_error("avcodec_alloc_context3 failed");
    }
    encoder_context_->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder_context_->codec_id = AV_CODEC_ID_H264;
    encoder_context_->width = kOutputWidth;
    encoder_context_->height = kOutputHeight;
    encoder_context_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder_context_->time_base = AVRational{1, kOutputFps};
    encoder_context_->framerate = AVRational{kOutputFps, 1};
    encoder_context_->bit_rate = kOutputBitrate;
    encoder_context_->gop_size = kOutputFps * 2;
    encoder_context_->max_b_frames = 0;
    encoder_context_->thread_count = 2;
    if (format_context_->oformat->flags & AVFMT_GLOBALHEADER) {
        encoder_context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    av_opt_set(encoder_context_->priv_data, "preset", "veryfast", 0);
    av_opt_set(encoder_context_->priv_data, "tune", "zerolatency", 0);
    check_ffmpeg(avcodec_open2(encoder_context_, encoder, nullptr),
                 "avcodec_open2");
    check_ffmpeg(
        avcodec_parameters_from_context(stream->codecpar, encoder_context_),
        "avcodec_parameters_from_context");
    stream->time_base = encoder_context_->time_base;

    if (!(format_context_->oformat->flags & AVFMT_NOFILE)) {
        check_ffmpeg(avio_open(&format_context_->pb, path_.c_str(), AVIO_FLAG_WRITE),
                     "avio_open");
    }
    check_ffmpeg(avformat_write_header(format_context_, nullptr),
                 "avformat_write_header");

    canvas_ = av_frame_alloc();
    if (!canvas_) {
        throw std::runtime_error("av_frame_alloc failed");
    }
    canvas_->format = encoder_context_->pix_fmt;
    canvas_->width = kOutputWidth;
    canvas_->height = kOutputHeight;
    check_ffmpeg(av_frame_get_buffer(canvas_, 32), "av_frame_get_buffer");
    next_pts_ = 0;

    std::cerr << "demo_recording_started=" << path_
              << " format=1280x720@30" << std::endl;
}

void DemoVideoRecorder::close_output() {
    if (encoder_context_ && format_context_) {
        if (avcodec_send_frame(encoder_context_, nullptr) >= 0) {
            AVPacket *packet = av_packet_alloc();
            if (packet) {
                while (avcodec_receive_packet(encoder_context_, packet) == 0) {
                    write_packet(packet);
                    av_packet_unref(packet);
                }
                av_packet_free(&packet);
            }
        }
        av_write_trailer(format_context_);
    }

    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
    av_frame_free(&canvas_);
    avcodec_free_context(&encoder_context_);
    if (format_context_) {
        if (!(format_context_->oformat->flags & AVFMT_NOFILE) &&
            format_context_->pb) {
            avio_closep(&format_context_->pb);
        }
        avformat_free_context(format_context_);
        format_context_ = nullptr;
    }
    stream_index_ = -1;
}

void DemoVideoRecorder::update_canvas(const AVFrame *source) {
    check_ffmpeg(av_frame_make_writable(canvas_), "av_frame_make_writable");
    for (int y = 0; y < kOutputHeight; ++y) {
        std::memset(canvas_->data[0] + y * canvas_->linesize[0], 16,
                    kOutputWidth);
    }
    for (int y = 0; y < kOutputHeight / 2; ++y) {
        std::memset(canvas_->data[1] + y * canvas_->linesize[1], 128,
                    kOutputWidth / 2);
        std::memset(canvas_->data[2] + y * canvas_->linesize[2], 128,
                    kOutputWidth / 2);
    }

    double scale = std::min(
        1.0, std::min(static_cast<double>(kOutputWidth) / source->width,
                      static_cast<double>(kOutputHeight) / source->height));
    int target_width =
        std::max(2, static_cast<int>(source->width * scale) & ~1);
    int target_height =
        std::max(2, static_cast<int>(source->height * scale) & ~1);
    int offset_x = ((kOutputWidth - target_width) / 2) & ~1;
    int offset_y = ((kOutputHeight - target_height) / 2) & ~1;

    sws_context_ = sws_getCachedContext(
        sws_context_, source->width, source->height,
        static_cast<AVPixelFormat>(source->format), target_width, target_height,
        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_context_) {
        throw std::runtime_error("sws_getCachedContext failed");
    }

    uint8_t *destination[4] = {
        canvas_->data[0] + offset_y * canvas_->linesize[0] + offset_x,
        canvas_->data[1] + (offset_y / 2) * canvas_->linesize[1] + offset_x / 2,
        canvas_->data[2] + (offset_y / 2) * canvas_->linesize[2] + offset_x / 2,
        nullptr,
    };
    const int destination_linesize[4] = {
        canvas_->linesize[0],
        canvas_->linesize[1],
        canvas_->linesize[2],
        0,
    };
    sws_scale(sws_context_, source->data, source->linesize, 0, source->height,
              destination, destination_linesize);
}

void DemoVideoRecorder::encode_canvas() {
    canvas_->pts = next_pts_++;
    check_ffmpeg(avcodec_send_frame(encoder_context_, canvas_),
                 "avcodec_send_frame");

    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        throw std::runtime_error("av_packet_alloc failed");
    }
    while (true) {
        const int result = avcodec_receive_packet(encoder_context_, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }
        if (result < 0) {
            const std::string error = ffmpeg_error(result);
            av_packet_free(&packet);
            throw std::runtime_error("avcodec_receive_packet failed: " + error);
        }
        write_packet(packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
}

void DemoVideoRecorder::write_packet(AVPacket *packet) {
    AVStream *stream = format_context_->streams[stream_index_];
    av_packet_rescale_ts(packet, encoder_context_->time_base, stream->time_base);
    packet->stream_index = stream_index_;
    check_ffmpeg(av_interleaved_write_frame(format_context_, packet),
                 "av_interleaved_write_frame");
}
