#pragma once

#include "video/video_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
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
    void set_frame_callback(std::function<void(const AVFrame *)> callback);

private:
    void open();
    void close();

    VideoRenderer &renderer_;
    std::function<void(const AVFrame *)> frame_callback_;
    AVCodecContext *context_ = nullptr;
    AVFrame *frame_ = nullptr;
};
