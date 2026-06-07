#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

struct VideoReceiveStatsSnapshot {
    uint64_t frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t nack_packets = 0;
    uint64_t retransmitted_packets = 0;
    uint64_t pli_packets = 0;
    uint64_t decoder_errors = 0;
    bool synchronized = false;
    double bitrate_kbps = 0.0;
};

class VideoReceiveStats {
public:
    void on_encoded_frame(std::size_t bytes);
    void on_decoder_error();
    void set_transport(uint64_t dropped_frames,
                       uint64_t nack_packets,
                       uint64_t retransmitted_packets,
                       uint64_t pli_packets,
                       bool synchronized);
    VideoReceiveStatsSnapshot snapshot() const;

private:
    using ByteWindow = std::deque<std::pair<int64_t, std::size_t>>;

    mutable std::mutex mutex_;
    uint64_t frames_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t nack_packets_ = 0;
    uint64_t retransmitted_packets_ = 0;
    uint64_t pli_packets_ = 0;
    uint64_t decoder_errors_ = 0;
    bool synchronized_ = false;
    ByteWindow bytes_window_;
};
