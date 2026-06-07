#pragma once

#include "quality/network_quality_estimator.hpp"

#include <rtc/mediahandler.hpp>
#include <rtc/rtp.hpp>

#include <deque>
#include <optional>

class MediaStatisticsHandler final : public rtc::MediaHandler {
public:
    enum class Direction {
        Sender,
        Receiver,
    };

    MediaStatisticsHandler(NetworkQualityEstimator &estimator, Direction direction);

    void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
    void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;

private:
    void observe_incoming(const rtc::message_ptr &message);
    void observe_outgoing(const rtc::message_ptr &message);
    void observe_rtp(const rtc::message_ptr &message, bool sent);
    void observe_rtcp(const rtc::message_ptr &message, bool sent);
    void observe_rtcp_packet(const std::byte *data, std::size_t size);
    void write_twcc_extension(const rtc::message_ptr &message);
    void observe_twcc_extension(const rtc::message_ptr &message);
    std::optional<uint16_t> read_twcc_sequence(const rtc::message_ptr &message) const;
    void maybe_send_twcc_feedback(const rtc::message_callback &send);
    void observe_report_block(const rtc::RtcpReportBlock &block);
    void observe_twcc_feedback(const std::byte *data, std::size_t size);

    struct TwccReceiveSample {
        uint16_t sequence = 0;
        int64_t at_us = 0;
    };

    NetworkQualityEstimator &estimator_;
    Direction direction_;
    int64_t last_twcc_feedback_us_ = 0;
    uint16_t next_twcc_sequence_ = 1;
    uint8_t twcc_feedback_count_ = 0;
    std::deque<TwccReceiveSample> pending_twcc_samples_;
};
