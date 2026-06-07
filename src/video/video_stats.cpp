#include "video/video_stats.hpp"

#include "common/utils.hpp"

void VideoReceiveStats::on_encoded_frame(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;

    const int64_t at_us = now_us();
    bytes_window_.emplace_back(at_us, bytes);
    const int64_t cutoff_us = at_us - 2'000'000;
    while (!bytes_window_.empty() && bytes_window_.front().first < cutoff_us) {
        bytes_window_.pop_front();
    }
}

void VideoReceiveStats::on_decoder_error() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++decoder_errors_;
}

void VideoReceiveStats::set_transport(uint64_t dropped_frames,
                                      uint64_t nack_packets,
                                      uint64_t retransmitted_packets,
                                      uint64_t pli_packets,
                                      bool synchronized) {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped_frames_ = dropped_frames;
    nack_packets_ = nack_packets;
    retransmitted_packets_ = retransmitted_packets;
    pli_packets_ = pli_packets;
    synchronized_ = synchronized;
}

VideoReceiveStatsSnapshot VideoReceiveStats::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    VideoReceiveStatsSnapshot result;
    result.frames = frames_;
    result.dropped_frames = dropped_frames_;
    result.nack_packets = nack_packets_;
    result.retransmitted_packets = retransmitted_packets_;
    result.pli_packets = pli_packets_;
    result.decoder_errors = decoder_errors_;
    result.synchronized = synchronized_;

    if (bytes_window_.size() >= 2) {
        std::size_t bytes = 0;
        for (const auto &[_, count] : bytes_window_) {
            bytes += count;
        }

        const double seconds =
            static_cast<double>(bytes_window_.back().first - bytes_window_.front().first) /
            1'000'000.0;
        if (seconds > 0.0) {
            result.bitrate_kbps = static_cast<double>(bytes * 8) / seconds / 1000.0;
        }
    }

    return result;
}
