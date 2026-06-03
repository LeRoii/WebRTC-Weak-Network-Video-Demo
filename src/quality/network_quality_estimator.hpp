#pragma once

#include "common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

class NetworkQualityEstimator {
public:
    void on_rtp_sent(uint16_t sequence, uint32_t rtp_timestamp, std::size_t bytes);
    void on_rtp_received(uint16_t sequence, uint32_t rtp_timestamp, std::size_t bytes);
    void on_rtcp_sent(std::size_t bytes);
    void on_rtcp_received(std::size_t bytes);
    void on_receiver_report(uint8_t fraction_lost,
                            uint32_t jitter_rtp_ticks,
                            uint32_t last_sr,
                            uint32_t delay_since_last_sr);
    void on_twcc_feedback(uint16_t base_sequence,
                          uint16_t packet_status_count,
                          uint32_t reference_time_24bit);
    void on_transport_loss_report(uint32_t expected_packets, uint32_t lost_packets);
    std::pair<uint32_t, uint32_t> transport_loss_report();

    NetworkQuality snapshot();

private:
    struct PacketSample {
        int64_t at_us = 0;
        uint16_t sequence = 0;
        uint32_t rtp_timestamp = 0;
        std::size_t bytes = 0;
    };

    struct RemoteReport {
        int64_t at_us = 0;
        double loss_percent = 0.0;
        double jitter_ms = 0.0;
        double rtt_ms = 0.0;
    };

    struct TwccReport {
        int64_t at_us = 0;
        uint16_t base_sequence = 0;
        uint16_t packet_status_count = 0;
        uint32_t reference_time_24bit = 0;
    };

    struct TransportLossSample {
        int64_t at_us = 0;
        uint32_t expected_packets = 0;
        uint32_t lost_packets = 0;
    };

    struct RemoteTransportLoss {
        int64_t at_us = 0;
        uint32_t expected_packets = 0;
        uint32_t lost_packets = 0;
    };

    using ByteWindow = std::deque<std::pair<int64_t, std::size_t>>;

    static void trim_byte_window(ByteWindow &window);
    static double bitrate_kbps(const ByteWindow &window);
    static uint32_t compact_ntp_now();

    void trim_packet_window(std::deque<PacketSample> &window) const;
    void trim_transport_loss_window();
    double local_loss_percent() const;
    double local_jitter_ms() const;
    double local_transport_loss_percent() const;

    mutable std::mutex mutex_;
    std::deque<PacketSample> sent_packets_;
    std::deque<PacketSample> received_packets_;
    ByteWindow sent_bytes_window_;
    ByteWindow received_bytes_window_;
    ByteWindow rtcp_bytes_window_;
    std::deque<TransportLossSample> transport_loss_samples_;
    std::optional<RemoteReport> remote_report_;
    std::optional<RemoteTransportLoss> remote_transport_loss_;
    std::optional<TwccReport> twcc_report_;
    std::optional<uint16_t> last_transport_sequence_;
};
