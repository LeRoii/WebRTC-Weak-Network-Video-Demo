#pragma once

#include "video/video_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVFrame;

class H264Decoder {
public:
    explicit H264Decoder(VideoRenderer &renderer);
    ~H264Decoder();

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool decode(const std::byte *data, std::size_t size);
    void reset();

private:
    void open();
    void close();

    VideoRenderer &renderer_;
    AVCodecContext *context_ = nullptr;
    AVFrame *frame_ = nullptr;
};
