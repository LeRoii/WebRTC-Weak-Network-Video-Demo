#include "quality/media_statistics_handler.hpp"

#include "common/utils.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint8_t kRtcpSenderReport = 200;
constexpr uint8_t kRtcpReceiverReport = 201;
constexpr uint8_t kRtcpTransportFeedback = 205;
constexpr uint8_t kTwccFeedbackFormat = 15;
constexpr uint8_t kTwccExtMapId = 3;
constexpr uint16_t kOneByteExtensionProfile = 0xbede;
constexpr int64_t kTwccFeedbackIntervalUs = 200'000;

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
        maybe_send_twcc_feedback(send);
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
        observe_twcc_extension(message);
        observe_rtp(message, false);
    } else {
        observe_rtcp(message, false);
    }
}

void MediaStatisticsHandler::observe_outgoing(const rtc::message_ptr &message) {
    if (direction_ == Direction::Sender) {
        write_twcc_extension(message);
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
        }

        offset += packet_size;
    }
}

void MediaStatisticsHandler::write_twcc_extension(const rtc::message_ptr &message) {
    if (!message || message->size() < sizeof(rtc::RtpHeader) || rtc::IsRtcp(*message)) {
        return;
    }

    const auto *old_header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
    if (old_header->version() != 2 || old_header->padding()) {
        return;
    }

    const std::size_t fixed_header_size = 12U + old_header->csrcCount() * 4U;
    if (message->size() < fixed_header_size) {
        return;
    }

    std::size_t old_payload_offset = fixed_header_size;
    std::vector<std::byte> extension_body;

    if (old_header->extension()) {
        if (message->size() < fixed_header_size + sizeof(rtc::RtpExtensionHeader)) {
            return;
        }
        const auto *extension = reinterpret_cast<const rtc::RtpExtensionHeader *>(
            message->data() + fixed_header_size);
        if (extension->profileSpecificId() != kOneByteExtensionProfile) {
            return;
        }
        const std::size_t extension_body_size =
            static_cast<std::size_t>(extension->headerLength()) * 4U;
        old_payload_offset += sizeof(rtc::RtpExtensionHeader) + extension_body_size;
        if (message->size() < old_payload_offset) {
            return;
        }

        const auto *body = message->data() + fixed_header_size +
                           sizeof(rtc::RtpExtensionHeader);
        extension_body.assign(body, body + extension_body_size);

        std::size_t used = 0;
        for (std::size_t offset = 0; offset < extension_body.size();) {
            const uint8_t header = std::to_integer<uint8_t>(extension_body[offset]);
            if (header == 0) {
                ++offset;
                continue;
            }
            const uint8_t id = header >> 4U;
            const std::size_t value_size = (header & 0x0fU) + 1U;
            if (id == 15 || offset + 1U + value_size > extension_body.size()) {
                break;
            }
            used = offset + 1U + value_size;
            offset = used;
        }
        extension_body.resize(used);
    }

    const uint16_t twcc_sequence = next_twcc_sequence_++;
    extension_body.push_back(static_cast<std::byte>((kTwccExtMapId << 4U) | 1U));
    extension_body.push_back(static_cast<std::byte>((twcc_sequence >> 8U) & 0xffU));
    extension_body.push_back(static_cast<std::byte>(twcc_sequence & 0xffU));
    while (extension_body.size() % 4U != 0U) {
        extension_body.push_back(std::byte{0});
    }

    std::vector<std::byte> rewritten;
    rewritten.reserve(message->size() + sizeof(rtc::RtpExtensionHeader) + 4U);
    rewritten.insert(rewritten.end(), message->begin(), message->begin() + fixed_header_size);
    rewritten[0] = static_cast<std::byte>(std::to_integer<uint8_t>(rewritten[0]) | 0x10U);

    const uint16_t profile = htons(kOneByteExtensionProfile);
    const uint16_t length_words =
        htons(static_cast<uint16_t>(extension_body.size() / 4U));
    const auto *profile_bytes = reinterpret_cast<const std::byte *>(&profile);
    const auto *length_bytes = reinterpret_cast<const std::byte *>(&length_words);
    rewritten.insert(rewritten.end(), profile_bytes, profile_bytes + sizeof(profile));
    rewritten.insert(rewritten.end(), length_bytes, length_bytes + sizeof(length_words));
    rewritten.insert(rewritten.end(), extension_body.begin(), extension_body.end());
    rewritten.insert(rewritten.end(), message->begin() + old_payload_offset, message->end());

    message->assign(rewritten.begin(), rewritten.end());
}

void MediaStatisticsHandler::observe_twcc_extension(const rtc::message_ptr &message) {
    if (const auto sequence = read_twcc_sequence(message)) {
        pending_twcc_samples_.push_back({*sequence, now_us()});
        while (pending_twcc_samples_.size() > 4096) {
            pending_twcc_samples_.pop_front();
        }
    }
}

std::optional<uint16_t> MediaStatisticsHandler::read_twcc_sequence(
    const rtc::message_ptr &message) const {
    if (!message || message->size() < sizeof(rtc::RtpHeader) || rtc::IsRtcp(*message)) {
        return std::nullopt;
    }

    const auto *header = reinterpret_cast<const rtc::RtpHeader *>(message->data());
    if (header->version() != 2 || !header->extension()) {
        return std::nullopt;
    }

    const std::size_t fixed_header_size = 12U + header->csrcCount() * 4U;
    if (message->size() < fixed_header_size + sizeof(rtc::RtpExtensionHeader)) {
        return std::nullopt;
    }

    const auto *extension = reinterpret_cast<const rtc::RtpExtensionHeader *>(
        message->data() + fixed_header_size);
    if (extension->profileSpecificId() != kOneByteExtensionProfile) {
        return std::nullopt;
    }

    const std::size_t extension_body_size =
        static_cast<std::size_t>(extension->headerLength()) * 4U;
    const std::size_t extension_body_offset =
        fixed_header_size + sizeof(rtc::RtpExtensionHeader);
    if (message->size() < extension_body_offset + extension_body_size) {
        return std::nullopt;
    }

    const auto *body = message->data() + extension_body_offset;
    for (std::size_t offset = 0; offset < extension_body_size;) {
        const uint8_t extension_header = std::to_integer<uint8_t>(body[offset]);
        if (extension_header == 0) {
            ++offset;
            continue;
        }

        const uint8_t id = extension_header >> 4U;
        const std::size_t value_size = (extension_header & 0x0fU) + 1U;
        if (id == 15 || offset + 1U + value_size > extension_body_size) {
            break;
        }
        if (id == kTwccExtMapId && value_size == 2U) {
            uint16_t sequence = 0;
            std::memcpy(&sequence, body + offset + 1U, sizeof(sequence));
            return ntohs(sequence);
        }
        offset += 1U + value_size;
    }

    return std::nullopt;
}

void MediaStatisticsHandler::maybe_send_twcc_feedback(const rtc::message_callback &send) {
    const int64_t at_us = now_us();
    if (at_us - last_twcc_feedback_us_ < kTwccFeedbackIntervalUs ||
        pending_twcc_samples_.empty()) {
        return;
    }
    last_twcc_feedback_us_ = at_us;

    const uint16_t base_sequence = pending_twcc_samples_.front().sequence;
    uint16_t max_sequence = base_sequence;
    for (const auto &sample : pending_twcc_samples_) {
        const auto delta = static_cast<int16_t>(sample.sequence - max_sequence);
        if (delta > 0) {
            max_sequence = sample.sequence;
        }
    }

    const uint16_t packet_count =
        static_cast<uint16_t>(static_cast<uint16_t>(max_sequence - base_sequence) + 1U);
    if (packet_count == 0 || packet_count > 4096) {
        pending_twcc_samples_.clear();
        return;
    }

    std::unordered_set<uint16_t> received_sequences;
    for (const auto &sample : pending_twcc_samples_) {
        received_sequences.insert(sample.sequence);
    }

    std::vector<uint16_t> chunks;
    std::size_t received_count = 0;
    for (uint16_t offset = 0; offset < packet_count;) {
        const uint16_t sequence = static_cast<uint16_t>(base_sequence + offset);
        const bool received = received_sequences.count(sequence) != 0;
        uint16_t run = 1;
        while (offset + run < packet_count && run < 0x1fffU) {
            const uint16_t next_sequence = static_cast<uint16_t>(base_sequence + offset + run);
            const bool next_received = received_sequences.count(next_sequence) != 0;
            if (next_received != received) {
                break;
            }
            ++run;
        }

        if (received) {
            received_count += run;
        }
        const uint16_t status_symbol = received ? 1U : 0U;
        chunks.push_back(static_cast<uint16_t>((status_symbol << 13U) | run));
        offset = static_cast<uint16_t>(offset + run);
    }

    const std::size_t packet_size = 20U + chunks.size() * sizeof(uint16_t) + received_count;
    const std::size_t padded_size = (packet_size + 3U) & ~std::size_t{3U};
    auto message = rtc::make_message(padded_size, rtc::Message::Control);
    std::fill(message->begin(), message->end(), std::byte{0});
    auto *bytes = message->data();

    bytes[0] = static_cast<std::byte>(0x80U | kTwccFeedbackFormat);
    bytes[1] = static_cast<std::byte>(kRtcpTransportFeedback);
    const uint16_t length_words = htons(static_cast<uint16_t>(padded_size / 4U - 1U));
    std::memcpy(bytes + 2, &length_words, sizeof(length_words));

    const uint32_t sender_ssrc = 0;
    const uint32_t media_ssrc = 0;
    std::memcpy(bytes + 4, &sender_ssrc, sizeof(sender_ssrc));
    std::memcpy(bytes + 8, &media_ssrc, sizeof(media_ssrc));

    const uint16_t base = htons(base_sequence);
    const uint16_t count = htons(packet_count);
    std::memcpy(bytes + 12, &base, sizeof(base));
    std::memcpy(bytes + 14, &count, sizeof(count));

    const uint32_t reference_time =
        static_cast<uint32_t>((pending_twcc_samples_.front().at_us / 1000 / 64) & 0x00ffffff);
    bytes[16] = static_cast<std::byte>((reference_time >> 16U) & 0xffU);
    bytes[17] = static_cast<std::byte>((reference_time >> 8U) & 0xffU);
    bytes[18] = static_cast<std::byte>(reference_time & 0xffU);
    bytes[19] = static_cast<std::byte>(twcc_feedback_count_++);

    std::size_t cursor = 20;
    for (const uint16_t chunk : chunks) {
        const uint16_t network_chunk = htons(chunk);
        std::memcpy(bytes + cursor, &network_chunk, sizeof(network_chunk));
        cursor += sizeof(network_chunk);
    }
    for (std::size_t i = 0; i < received_count; ++i) {
        bytes[cursor++] = std::byte{0};
    }

    send(message);
    pending_twcc_samples_.clear();
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

    std::size_t cursor = 20;
    uint16_t parsed_packets = 0;
    uint32_t lost_packets = 0;
    uint32_t received_packets = 0;

    while (parsed_packets < ntohs(status_count) && cursor + sizeof(uint16_t) <= size) {
        uint16_t chunk = 0;
        std::memcpy(&chunk, data + cursor, sizeof(chunk));
        chunk = ntohs(chunk);
        cursor += sizeof(chunk);

        if ((chunk & 0x8000U) == 0) {
            const uint16_t status = (chunk >> 13U) & 0x03U;
            const uint16_t run = chunk & 0x1fffU;
            const uint16_t remaining = static_cast<uint16_t>(ntohs(status_count) - parsed_packets);
            const uint16_t count = std::min(run, remaining);
            if (status == 0) {
                lost_packets += count;
            } else {
                received_packets += count;
            }
            parsed_packets = static_cast<uint16_t>(parsed_packets + count);
        } else {
            const bool two_bit = (chunk & 0x4000U) != 0;
            const int symbol_count = two_bit ? 7 : 14;
            for (int i = 0; i < symbol_count && parsed_packets < ntohs(status_count); ++i) {
                const int shift = two_bit ? (12 - i * 2) : (13 - i);
                const uint16_t status = two_bit ? ((chunk >> shift) & 0x03U)
                                                : ((chunk >> shift) & 0x01U);
                if (status == 0) {
                    ++lost_packets;
                } else {
                    ++received_packets;
                }
                ++parsed_packets;
            }
        }
    }

    const uint32_t expected_packets = received_packets + lost_packets;
    if (expected_packets > 0) {
        estimator_.on_transport_loss_report(expected_packets, lost_packets);
    }
}
