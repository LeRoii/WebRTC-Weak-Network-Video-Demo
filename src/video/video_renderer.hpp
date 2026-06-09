#pragma once

#include "video/frame_timing.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

struct AVFrame;

class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    VideoRenderer(const VideoRenderer &) = delete;
    VideoRenderer &operator=(const VideoRenderer &) = delete;

    void start();
    void stop();
    void submit(const AVFrame *frame, FrameTiming timing);
    void on_present(
        std::function<void(const AVFrame *, FrameTiming)> callback);
    void on_drop(std::function<void(FrameTiming)> callback);

private:
    struct QueuedFrame {
        AVFrame *frame = nullptr;
        FrameTiming timing;
    };

    void render_loop();
    void clear_queue();

    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedFrame> frames_;
    std::function<void(const AVFrame *, FrameTiming)> present_callback_;
    std::function<void(FrameTiming)> drop_callback_;
    std::thread thread_;
};
