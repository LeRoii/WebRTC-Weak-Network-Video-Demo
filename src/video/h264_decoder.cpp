#include "video/h264_decoder.hpp"

#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

H264Decoder::H264Decoder(VideoRenderer &renderer) : renderer_(renderer) {
    open();
}

H264Decoder::~H264Decoder() {
    close();
}

void H264Decoder::open() {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        throw std::runtime_error("FFmpeg H264 decoder not found");
    }

    context_ = avcodec_alloc_context3(codec);
    if (!context_) {
        throw std::runtime_error("avcodec_alloc_context3 failed");
    }
    context_->err_recognition = AV_EF_EXPLODE | AV_EF_CAREFUL;
    context_->thread_count = 1;

    if (avcodec_open2(context_, codec, nullptr) < 0) {
        throw std::runtime_error("avcodec_open2 failed");
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("av_frame_alloc failed");
    }
}

void H264Decoder::close() {
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
}

void H264Decoder::reset() {
    close();
    open();
}

bool H264Decoder::decode(const std::byte *data, std::size_t size) {
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        return false;
    }
    std::memcpy(packet->data, data, size);

    const int send_result = avcodec_send_packet(context_, packet);
    av_packet_free(&packet);
    if (send_result < 0) {
        return false;
    }

    bool rendered = false;
    while (avcodec_receive_frame(context_, frame_) == 0) {
        const bool corrupt =
            (frame_->flags & AV_FRAME_FLAG_CORRUPT) != 0 ||
            frame_->decode_error_flags != 0;
        if (!corrupt) {
            renderer_.submit(frame_);
            rendered = true;
        }
        av_frame_unref(frame_);
    }
    return rendered;
}
