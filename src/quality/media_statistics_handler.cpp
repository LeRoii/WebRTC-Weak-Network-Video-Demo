#include "quality/media_statistics_handler.hpp"

#include "common/utils.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstddef>
#include <cstring>

namespace {
constexpr uint8_t kRtcpSenderReport = 200;
constexpr uint8_t kRtcpReceiverReport = 201;
constexpr uint8_t kRtcpApp = 204;
constexpr uint8_t kRtcpTransportFeedback = 205;
constexpr uint8_t kTwccFeedbackFormat = 15;
constexpr int64_t kTransportLossReportIntervalUs = 1'000'000;
constexpr uint32_t kTransportLossReportName = 0x4e514c53U; // NQLS
constexpr std::size_t kTransportLossReportSize = 20;

template <typename T>
const T *as_packet(const std::byte *data, std::size_t size) {
    if (size < sizeof(T)) {
        return nullptr;
    }
    return reinterpret_cast<const T *>(data);
}
}

MediaStatisticsHandler::MediaStatisticsHandler(NetworkQualityEstimator &estimator,
                                               Direction direction)
    : estimator_(estimator), direction_(direction) {}

void MediaStatisticsHandler::incoming(rtc::message_vector &messages,
                                      const rtc::message_callback &send) {
    for (const auto &message : messages) {
        observe_incoming(message);
    }
    if (direction_ == Direction::Receiver) {
        maybe_send_transport_loss_report(send);
    }
}

void MediaStatisticsHandler::outgoing(rtc::message_vector &messages,
                                      const rtc::message_callback &send) {
    (void)send;
    for (const auto &message : messages) {
        observe_outgoing(message);
    }
}

void MediaStatisticsHandler::observe_incoming(const rtc::message_ptr &message) {
    if (direction_ == Direction::Receiver) {
        observe_rtp(message, false);
    } else {
        observe_rtcp(message, false);
    }
}

void MediaStatisticsHandler::observe_outgoing(const rtc::message_ptr &message) {
    if (direction_ == Direction::Sender) {
        observe_rtp(message, true);
    } else {
        observe_rtcp(message, true);
    }
}

void MediaStatisticsHandler::observe_rtp(const rtc::message_ptr &message, bool sent) {
    if (!message || message->size() < sizeof(rtc::RtpHeader) || rtc::IsRtcp(*message)) {
        return;
    }

    const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
    if (header->version() != 2) {
        return;
    }

    if (sent) {
        estimator_.on_rtp_sent(header->seqNumber(), header->timestamp(), message->size());
    } else {
        estimator_.on_rtp_received(header->seqNumber(), header->timestamp(), message->size());
    }
}

void MediaStatisticsHandler::observe_rtcp(const rtc::message_ptr &message, bool sent) {
    if (!message || message->size() < sizeof(rtc::RtcpHeader) || !rtc::IsRtcp(*message)) {
        return;
    }

    if (sent) {
        estimator_.on_rtcp_sent(message->size());
    } else {
        estimator_.on_rtcp_received(message->size());
        observe_rtcp_packet(message->data(), message->size());
    }
}

void MediaStatisticsHandler::observe_rtcp_packet(const std::byte *data, std::size_t size) {
    std::size_t offset = 0;
    while (offset + sizeof(rtc::RtcpHeader) <= size) {
        const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(data + offset);
        const std::size_t packet_size = header->lengthInBytes();
        if (packet_size < sizeof(rtc::RtcpHeader) || offset + packet_size > size) {
            return;
        }

        const std::byte *packet = data + offset;
        if (header->payloadType() == kRtcpReceiverReport) {
            if (const auto *rr = as_packet<rtc::RtcpRr>(packet, packet_size)) {
                const int report_count = rr->header.reportCount();
                for (int i = 0; i < report_count; ++i) {
                    if (const auto *block = rr->getReportBlock(i)) {
                        observe_report_block(*block);
                    }
                }
            }
        } else if (header->payloadType() == kRtcpSenderReport) {
            if (const auto *sr = as_packet<rtc::RtcpSr>(packet, packet_size)) {
                const int report_count = sr->header.reportCount();
                for (int i = 0; i < report_count; ++i) {
                    if (const auto *block = sr->getReportBlock(i)) {
                        observe_report_block(*block);
                    }
                }
            }
        } else if (header->payloadType() == kRtcpTransportFeedback &&
                   header->reportCount() == kTwccFeedbackFormat) {
            observe_twcc_feedback(packet, packet_size);
        } else if (header->payloadType() == kRtcpApp) {
            observe_transport_loss_report(packet, packet_size);
        }

        offset += packet_size;
    }
}

void MediaStatisticsHandler::maybe_send_transport_loss_report(
    const rtc::message_callback &send) {
    const int64_t at_us = now_us();
    if (at_us - last_transport_loss_report_us_ < kTransportLossReportIntervalUs) {
        return;
    }

    const auto [expected_packets, lost_packets] = estimator_.transport_loss_report();
    if (expected_packets == 0) {
        return;
    }
    last_transport_loss_report_us_ = at_us;

    auto message = rtc::make_message(kTransportLossReportSize, rtc::Message::Control);
    auto *bytes = message->data();
    bytes[0] = static_cast<std::byte>(0x80);
    bytes[1] = static_cast<std::byte>(kRtcpApp);

    const uint16_t length_words = htons(
        static_cast<uint16_t>(kTransportLossReportSize / 4U - 1U));
    std::memcpy(bytes + 2, &length_words, sizeof(length_words));

    const uint32_t sender_ssrc = 0;
    std::memcpy(bytes + 4, &sender_ssrc, sizeof(sender_ssrc));

    const uint32_t name = htonl(kTransportLossReportName);
    std::memcpy(bytes + 8, &name, sizeof(name));

    const uint32_t expected = htonl(expected_packets);
    const uint32_t lost = htonl(lost_packets);
    std::memcpy(bytes + 12, &expected, sizeof(expected));
    std::memcpy(bytes + 16, &lost, sizeof(lost));

    send(message);
}

void MediaStatisticsHandler::observe_transport_loss_report(const std::byte *data,
                                                           std::size_t size) {
    if (size < kTransportLossReportSize) {
        return;
    }

    uint32_t name = 0;
    std::memcpy(&name, data + 8, sizeof(name));
    if (ntohl(name) != kTransportLossReportName) {
        return;
    }

    uint32_t expected = 0;
    uint32_t lost = 0;
    std::memcpy(&expected, data + 12, sizeof(expected));
    std::memcpy(&lost, data + 16, sizeof(lost));
    estimator_.on_transport_loss_report(ntohl(expected), ntohl(lost));
}

void MediaStatisticsHandler::observe_report_block(const rtc::RtcpReportBlock &block) {
    estimator_.on_receiver_report(block.getFractionLost(),
                                  block.jitter(),
                                  block.getNTPOfSR(),
                                  block.delaySinceSR());
}

void MediaStatisticsHandler::observe_twcc_feedback(const std::byte *data, std::size_t size) {
    if (size < 20) {
        return;
    }

    uint16_t base_sequence = 0;
    uint16_t status_count = 0;
    std::memcpy(&base_sequence, data + 12, sizeof(base_sequence));
    std::memcpy(&status_count, data + 14, sizeof(status_count));

    const uint32_t reference_time =
        (static_cast<uint32_t>(std::to_integer<uint8_t>(data[16])) << 16U) |
        (static_cast<uint32_t>(std::to_integer<uint8_t>(data[17])) << 8U) |
        static_cast<uint32_t>(std::to_integer<uint8_t>(data[18]));

    estimator_.on_twcc_feedback(ntohs(base_sequence), ntohs(status_count), reference_time);
}
