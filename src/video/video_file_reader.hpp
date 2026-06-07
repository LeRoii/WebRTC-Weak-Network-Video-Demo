#pragma once

#include "common/types.hpp"

#include <cstdint>
#include <rtc/rtc.hpp>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVFormatContext;
struct SwsContext;

struct EncodedVideoFrame {
    rtc::binary data;
    uint32_t duration_90khz = 3000;
    uint16_t epoch = 1;
    bool keyframe = false;
};

class VideoFileReader {
public:
    explicit VideoFileReader(std::string path);
    ~VideoFileReader();

    VideoFileReader(const VideoFileReader &) = delete;
    VideoFileReader &operator=(const VideoFileReader &) = delete;

    bool next_frame(EncodedVideoFrame &frame,
                    const VideoProfile &profile,
                    bool force_keyframe);
    void reset();

private:
    void open();
    void close();
    void close_encoder();
    void configure_encoder(const VideoProfile &profile);
    bool next_decoded_frame();
    bool should_output_decoded_frame(int fps);

    std::string path_;
    AVFormatContext *format_context_ = nullptr;
    AVCodecContext *decoder_context_ = nullptr;
    AVCodecContext *encoder_context_ = nullptr;
    AVFrame *decoded_frame_ = nullptr;
    AVFrame *scaled_frame_ = nullptr;
    SwsContext *sws_context_ = nullptr;
    int video_stream_index_ = -1;
    int stream_time_base_num_ = 0;
    int stream_time_base_den_ = 1;
    bool input_eof_ = false;
    bool decoder_flushed_ = false;
    bool encoder_configured_ = false;
    VideoProfile encoder_profile_;
    uint16_t epoch_ = 0;
    int64_t encoder_pts_ = 0;
    double next_output_source_seconds_ = -1.0;
};
