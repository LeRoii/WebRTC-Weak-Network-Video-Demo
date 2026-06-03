#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
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
    void submit(const AVFrame *frame);

private:
    void render_loop();
    void clear_queue();

    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AVFrame *> frames_;
    std::thread thread_;
};
