#pragma once

#include "quality/network_quality_estimator.hpp"

#include <rtc/mediahandler.hpp>
#include <rtc/rtp.hpp>

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
    void maybe_send_transport_loss_report(const rtc::message_callback &send);
    void observe_transport_loss_report(const std::byte *data, std::size_t size);
    void observe_report_block(const rtc::RtcpReportBlock &block);
    void observe_twcc_feedback(const std::byte *data, std::size_t size);

    NetworkQualityEstimator &estimator_;
    Direction direction_;
    int64_t last_transport_loss_report_us_ = 0;
};
