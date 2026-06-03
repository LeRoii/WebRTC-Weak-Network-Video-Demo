#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

struct VideoReceiveStatsSnapshot {
    uint64_t frames = 0;
    double bitrate_kbps = 0.0;
};

class VideoReceiveStats {
public:
    void on_encoded_frame(std::size_t bytes);
    VideoReceiveStatsSnapshot snapshot() const;

private:
    using ByteWindow = std::deque<std::pair<int64_t, std::size_t>>;

    mutable std::mutex mutex_;
    uint64_t frames_ = 0;
    ByteWindow bytes_window_;
};
