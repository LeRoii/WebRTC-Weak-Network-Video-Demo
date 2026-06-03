#include "video/h264_decoder.hpp"

#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

H264Decoder::H264Decoder(VideoRenderer &renderer) : renderer_(renderer) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        throw std::runtime_error("FFmpeg H264 decoder not found");
    }

    context_ = avcodec_alloc_context3(codec);
    if (!context_) {
        throw std::runtime_error("avcodec_alloc_context3 failed");
    }

    if (avcodec_open2(context_, codec, nullptr) < 0) {
        throw std::runtime_error("avcodec_open2 failed");
    }

    frame_ = av_frame_alloc();
    if (!frame_) {
        throw std::runtime_error("av_frame_alloc failed");
    }
}

H264Decoder::~H264Decoder() {
    av_frame_free(&frame_);
    avcodec_free_context(&context_);
}

void H264Decoder::decode(const std::byte *data, std::size_t size) {
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        return;
    }

    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        return;
    }
    std::memcpy(packet->data, data, size);

    const int send_result = avcodec_send_packet(context_, packet);
    av_packet_free(&packet);
    if (send_result < 0) {
        return;
    }

    while (avcodec_receive_frame(context_, frame_) == 0) {
        renderer_.submit(frame_);
        av_frame_unref(frame_);
    }
}
