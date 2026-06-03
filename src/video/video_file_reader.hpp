#pragma once

#include <cstdint>
#include <memory>
#include <rtc/rtc.hpp>
#include <string>

struct AVBSFContext;
struct AVFormatContext;

struct EncodedVideoFrame {
    rtc::binary data;
    uint32_t duration_90khz = 3000;
};

class VideoFileReader {
public:
    explicit VideoFileReader(std::string path);
    ~VideoFileReader();

    VideoFileReader(const VideoFileReader &) = delete;
    VideoFileReader &operator=(const VideoFileReader &) = delete;

    bool next_frame(EncodedVideoFrame &frame);
    void reset();

private:
    void open();
    void close();

    std::string path_;
    AVFormatContext *format_context_ = nullptr;
    AVBSFContext *bsf_context_ = nullptr;
    int video_stream_index_ = -1;
    uint32_t fallback_duration_90khz_ = 3000;
    bool input_eof_ = false;
};
