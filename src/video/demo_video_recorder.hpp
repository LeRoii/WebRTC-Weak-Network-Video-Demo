#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class DemoVideoRecorder {
public:
    DemoVideoRecorder();
    ~DemoVideoRecorder();

    DemoVideoRecorder(const DemoVideoRecorder &) = delete;
    DemoVideoRecorder &operator=(const DemoVideoRecorder &) = delete;

    void start(std::string path);
    void stop();
    void submit(const AVFrame *frame);
    bool active() const;

private:
    void record_loop();
    void open_output();
    void close_output();
    void update_canvas(const AVFrame *source);
    void encode_canvas();
    void write_packet(AVPacket *packet);

    std::atomic<bool> stopping_{false};
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    AVFrame *latest_source_ = nullptr;
    uint64_t source_version_ = 0;
    std::thread thread_;
    std::string path_;

    AVFormatContext *format_context_ = nullptr;
    AVCodecContext *encoder_context_ = nullptr;
    AVFrame *canvas_ = nullptr;
    SwsContext *sws_context_ = nullptr;
    int stream_index_ = -1;
    int64_t next_pts_ = 0;
};
