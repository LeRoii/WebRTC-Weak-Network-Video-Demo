#include "quality/network_quality_estimator.hpp"

#include "common/utils.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {
constexpr int64_t kStatsWindowUs = 2'000'000;
constexpr int64_t kRemoteReportFreshUs = 3'000'000;
constexpr int64_t kTransportLossReportFreshUs = 3'000'000;
constexpr double kVideoClockRate = 90000.0;
constexpr uint64_t kNtpUnixEpochDelta = 2208988800ULL;
}

void NetworkQualityEstimator::on_rtp_sent(uint16_t sequence,
                                          uint32_t rtp_timestamp,
                                          std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t at_us = now_us();
    sent_packets_.push_back({at_us, sequence, rtp_timestamp, bytes});
    sent_bytes_window_.emplace_back(at_us, bytes);
    trim_packet_window(sent_packets_);
    trim_byte_window(sent_bytes_window_);
}

void NetworkQualityEstimator::on_rtp_received(uint16_t sequence,
                                              uint32_t rtp_timestamp,
                                              std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t at_us = now_us();

    uint32_t expected_packets = 1;
    uint32_t lost_packets = 0;
    if (last_transport_sequence_) {
        const auto delta =
            static_cast<int16_t>(sequence - *last_transport_sequence_);
        if (delta > 0) {
            expected_packets = static_cast<uint32_t>(delta);
            lost_packets = static_cast<uint32_t>(delta - 1);
            last_transport_sequence_ = sequence;
        } else {
            expected_packets = 0;
        }
    } else {
        last_transport_sequence_ = sequence;
    }
    if (expected_packets > 0) {
        transport_loss_samples_.push_back({at_us, expected_packets, lost_packets});
        trim_transport_loss_window();
    }

    received_packets_.push_back({at_us, sequence, rtp_timestamp, bytes});
    received_bytes_window_.emplace_back(at_us, bytes);
    trim_packet_window(received_packets_);
    trim_byte_window(received_bytes_window_);
}

void NetworkQualityEstimator::on_rtcp_sent(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtcp_bytes_window_.emplace_back(now_us(), bytes);
    trim_byte_window(rtcp_bytes_window_);
}

void NetworkQualityEstimator::on_rtcp_received(std::size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtcp_bytes_window_.emplace_back(now_us(), bytes);
    trim_byte_window(rtcp_bytes_window_);
}

void NetworkQualityEstimator::on_receiver_report(uint8_t fraction_lost,
                                                 uint32_t jitter_rtp_ticks,
                                                 uint32_t last_sr,
                                                 uint32_t delay_since_last_sr) {
    std::lock_guard<std::mutex> lock(mutex_);

    double rtt_ms = 0.0;
    if (last_sr != 0) {
        const uint32_t arrival = compact_ntp_now();
        const uint32_t rtt_ntp = arrival - last_sr - delay_since_last_sr;
        rtt_ms = static_cast<double>(rtt_ntp) * 1000.0 / 65536.0;
        if (rtt_ms < 0.0 || rtt_ms > 60'000.0) {
            rtt_ms = 0.0;
        }
    }

    remote_report_ = RemoteReport{
        now_us(),
        static_cast<double>(fraction_lost) * 100.0 / 256.0,
        static_cast<double>(jitter_rtp_ticks) * 1000.0 / kVideoClockRate,
        rtt_ms,
    };
}

void NetworkQualityEstimator::on_twcc_feedback(uint16_t base_sequence,
                                               uint16_t packet_status_count,
                                               uint32_t reference_time_24bit) {
    std::lock_guard<std::mutex> lock(mutex_);
    twcc_report_ = TwccReport{
        now_us(),
        base_sequence,
        packet_status_count,
        reference_time_24bit,
    };
}

void NetworkQualityEstimator::on_transport_loss_report(uint32_t expected_packets,
                                                       uint32_t lost_packets) {
    std::lock_guard<std::mutex> lock(mutex_);
    remote_transport_loss_ = RemoteTransportLoss{
        now_us(),
        expected_packets,
        std::min(lost_packets, expected_packets),
    };
}

std::pair<uint32_t, uint32_t> NetworkQualityEstimator::transport_loss_report() {
    std::lock_guard<std::mutex> lock(mutex_);
    trim_transport_loss_window();

    uint32_t expected_packets = 0;
    uint32_t lost_packets = 0;
    for (const auto &sample : transport_loss_samples_) {
        expected_packets += sample.expected_packets;
        lost_packets += sample.lost_packets;
    }
    return {expected_packets, lost_packets};
}

NetworkQuality NetworkQualityEstimator::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    trim_packet_window(sent_packets_);
    trim_packet_window(received_packets_);
    trim_byte_window(sent_bytes_window_);
    trim_byte_window(received_bytes_window_);
    trim_byte_window(rtcp_bytes_window_);
    trim_transport_loss_window();

    NetworkQuality quality;
    quality.loss_percent = local_loss_percent();
    quality.jitter_ms = local_jitter_ms();
    quality.transport_loss_percent = local_transport_loss_percent();
    quality.send_kbps = bitrate_kbps(sent_bytes_window_);
    quality.receive_kbps = bitrate_kbps(received_bytes_window_);
    quality.ack_kbps = bitrate_kbps(rtcp_bytes_window_);

    if (remote_report_ && now_us() - remote_report_->at_us < kRemoteReportFreshUs) {
        quality.rtt_ms = remote_report_->rtt_ms;
        quality.loss_percent = remote_report_->loss_percent;
        quality.jitter_ms = remote_report_->jitter_ms;
    }
    if (remote_transport_loss_ &&
        now_us() - remote_transport_loss_->at_us < kTransportLossReportFreshUs &&
        remote_transport_loss_->expected_packets > 0) {
        quality.transport_loss_percent =
            static_cast<double>(remote_transport_loss_->lost_packets) * 100.0 /
            static_cast<double>(remote_transport_loss_->expected_packets);
    }

    return quality;
}

void NetworkQualityEstimator::trim_byte_window(ByteWindow &window) {
    const int64_t cutoff_us = now_us() - kStatsWindowUs;
    while (!window.empty() && window.front().first < cutoff_us) {
        window.pop_front();
    }
}

double NetworkQualityEstimator::bitrate_kbps(const ByteWindow &window) {
    if (window.size() < 2) {
        return 0.0;
    }

    std::size_t total_bytes = 0;
    for (const auto &[_, bytes] : window) {
        total_bytes += bytes;
    }

    const double seconds =
        static_cast<double>(window.back().first - window.front().first) / 1'000'000.0;
    if (seconds <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(total_bytes * 8) / seconds / 1000.0;
}

uint32_t NetworkQualityEstimator::compact_ntp_now() {
    const uint64_t unix_us = static_cast<uint64_t>(now_us());
    const uint64_t ntp_seconds = unix_us / 1'000'000ULL + kNtpUnixEpochDelta;
    const uint64_t ntp_fraction =
        ((unix_us % 1'000'000ULL) << 32U) / 1'000'000ULL;
    const uint64_t ntp = (ntp_seconds << 32U) | ntp_fraction;
    return static_cast<uint32_t>((ntp >> 16U) & 0xffffffffU);
}

void NetworkQualityEstimator::trim_packet_window(std::deque<PacketSample> &window) const {
    const int64_t cutoff_us = now_us() - kStatsWindowUs;
    while (!window.empty() && window.front().at_us < cutoff_us) {
        window.pop_front();
    }
}

void NetworkQualityEstimator::trim_transport_loss_window() {
    const int64_t cutoff_us = now_us() - kStatsWindowUs;
    while (!transport_loss_samples_.empty() &&
           transport_loss_samples_.front().at_us < cutoff_us) {
        transport_loss_samples_.pop_front();
    }
}

double NetworkQualityEstimator::local_loss_percent() const {
    if (received_packets_.size() < 2) {
        return 0.0;
    }

    uint16_t min_sequence = received_packets_.front().sequence;
    uint16_t max_sequence = received_packets_.front().sequence;
    std::unordered_set<uint16_t> received;

    for (const auto &sample : received_packets_) {
        min_sequence = std::min(min_sequence, sample.sequence);
        max_sequence = std::max(max_sequence, sample.sequence);
        received.insert(sample.sequence);
    }

    const uint32_t expected =
        static_cast<uint32_t>(max_sequence) - static_cast<uint32_t>(min_sequence) + 1U;
    if (expected == 0U || received.size() > expected) {
        return 0.0;
    }

    const uint32_t lost = expected - static_cast<uint32_t>(received.size());
    return static_cast<double>(lost) * 100.0 / static_cast<double>(expected);
}

double NetworkQualityEstimator::local_transport_loss_percent() const {
    uint32_t expected_packets = 0;
    uint32_t lost_packets = 0;
    for (const auto &sample : transport_loss_samples_) {
        expected_packets += sample.expected_packets;
        lost_packets += sample.lost_packets;
    }
    if (expected_packets == 0) {
        return 0.0;
    }
    return static_cast<double>(lost_packets) * 100.0 /
           static_cast<double>(expected_packets);
}

double NetworkQualityEstimator::local_jitter_ms() const {
    if (received_packets_.size() < 3) {
        return 0.0;
    }

    double jitter = 0.0;
    for (std::size_t i = 1; i < received_packets_.size(); ++i) {
        const auto &previous = received_packets_[i - 1];
        const auto &current = received_packets_[i];
        const double transit_previous =
            static_cast<double>(previous.at_us) / 1'000'000.0 -
            static_cast<double>(previous.rtp_timestamp) / kVideoClockRate;
        const double transit_current =
            static_cast<double>(current.at_us) / 1'000'000.0 -
            static_cast<double>(current.rtp_timestamp) / kVideoClockRate;
        const double delta_ms = std::abs(transit_current - transit_previous) * 1000.0;
        jitter += (delta_ms - jitter) / 16.0;
    }

    return jitter;
}
