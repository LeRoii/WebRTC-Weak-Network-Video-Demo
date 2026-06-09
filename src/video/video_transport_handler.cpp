#include "video/video_transport_handler.hpp"

#include "common/utils.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <set>

namespace {
constexpr uint8_t kFrameInfoExtMapId = 4;
constexpr uint16_t kOneByteExtensionProfile = 0xbede;
constexpr std::size_t kFrameMetadataSize = 16;
constexpr int64_t kNackDelayUs = 120'000;
constexpr int64_t kNackRetryUs = 50'000;
constexpr std::size_t kMaxBufferedFrames = 64;
constexpr uint8_t kRtcpTransportFeedback = 205;
constexpr uint8_t kRtcpPayloadFeedback = 206;
constexpr uint8_t kRtcpApp = 204;
constexpr uint8_t kNackFeedbackFormat = 1;
constexpr uint8_t kPliFeedbackFormat = 1;
constexpr uint32_t kKeyframeAckName = 0x4b464f4bU; // KFOK
constexpr std::size_t kKeyframeAckSize = 20;

uint16_t read_u16(const std::byte *data) {
    uint16_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

uint32_t read_u32(const std::byte *data) {
    uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

void append_u16(std::vector<std::byte> &output, uint16_t value) {
    value = htons(value);
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(value));
}

void append_u32(std::vector<std::byte> &output, uint32_t value) {
    value = htonl(value);
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    output.insert(output.end(), bytes, bytes + sizeof(value));
}

bool is_rtp(const rtc::message_ptr &message) {
    return message && message->type != rtc::Message::Control &&
           message->size() >= sizeof(rtc::RtpHeader) && !rtc::IsRtcp(*message);
}
}

RtxReceivingHandler::RtxReceivingHandler(uint8_t primary_payload_type,
                                         uint8_t rtx_payload_type,
                                         uint32_t primary_ssrc,
                                         uint32_t rtx_ssrc)
    : primary_payload_type_(primary_payload_type),
      rtx_payload_type_(rtx_payload_type),
      primary_ssrc_(primary_ssrc),
      rtx_ssrc_(rtx_ssrc) {}

void RtxReceivingHandler::incoming(rtc::message_vector &messages,
                                   const rtc::message_callback &) {
    for (const auto &message : messages) {
        if (!is_rtp(message)) {
            continue;
        }

        auto *header = reinterpret_cast<rtc::RtpHeader *>(message->data());
        if (header->payloadType() != rtx_payload_type_ ||
            header->ssrc() != rtx_ssrc_) {
            continue;
        }

        const std::size_t fixed_header_size =
            12U + static_cast<std::size_t>(header->csrcCount()) * 4U;
        if (message->size() < fixed_header_size) {
            continue;
        }

        std::size_t payload_offset = fixed_header_size;
        if (header->extension()) {
            if (message->size() <
                fixed_header_size + sizeof(rtc::RtpExtensionHeader)) {
                continue;
            }
            const auto *extension =
                reinterpret_cast<const rtc::RtpExtensionHeader *>(
                    message->data() + fixed_header_size);
            payload_offset += sizeof(rtc::RtpExtensionHeader) +
                              static_cast<std::size_t>(
                                  extension->headerLength()) *
                                  4U;
        }
        if (message->size() < payload_offset + sizeof(uint16_t)) {
            continue;
        }

        const uint16_t original_sequence =
            read_u16(message->data() + payload_offset);
        std::memmove(message->data() + payload_offset,
                     message->data() + payload_offset + sizeof(uint16_t),
                     message->size() - payload_offset - sizeof(uint16_t));
        message->resize(message->size() - sizeof(uint16_t));

        header = reinterpret_cast<rtc::RtpHeader *>(message->data());
        header->setPayloadType(primary_payload_type_);
        header->setSeqNumber(original_sequence);
        header->setSsrc(primary_ssrc_);
        message->stream = primary_ssrc_;
        retransmitted_packets_.fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t RtxReceivingHandler::retransmitted_packets() const {
    return retransmitted_packets_.load(std::memory_order_relaxed);
}

bool VideoTransportHandler::FrameAssembly::complete() const {
    if (packet_count == 0 || packets.size() != packet_count) {
        return false;
    }
    return std::all_of(packets.begin(), packets.end(),
                       [](const auto &packet) { return packet != nullptr; });
}

VideoTransportHandler::VideoTransportHandler(Direction direction,
                                             int recovery_timeout_ms)
    : direction_(direction),
      recovery_timeout_us_(
          std::max<int64_t>(100, recovery_timeout_ms) * 1000) {
    stats_.synchronized = direction == Direction::Sender;
}

void VideoTransportHandler::incoming(rtc::message_vector &messages,
                                     const rtc::message_callback &send) {
    if (direction_ == Direction::Sender) {
        sender_incoming(messages, send);
    } else {
        receiver_incoming(messages, send);
    }
}

void VideoTransportHandler::outgoing(rtc::message_vector &messages,
                                     const rtc::message_callback &) {
    if (direction_ == Direction::Sender) {
        sender_outgoing(messages);
    }
}

void VideoTransportHandler::set_outgoing_frame(uint16_t epoch,
                                               bool keyframe) {
    set_outgoing_frame(epoch, keyframe, 0);
}

void VideoTransportHandler::set_outgoing_frame(
    uint16_t epoch,
    bool keyframe,
    uint32_t sender_start_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = std::max<uint16_t>(1, epoch);
    next_frame_keyframe_ = keyframe;
    next_frame_sender_start_us_ = sender_start_us;
}

void VideoTransportHandler::poll() {
    rtc::message_callback send;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        send = receiver_send_;
    }
    if (direction_ == Direction::Receiver && send) {
        receiver_poll(send);
    }
}

void VideoTransportHandler::invalidate_sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (direction_ == Direction::Receiver) {
        for (const auto &entry : frames_) {
            record_network_drop(entry.second);
        }
        frames_.clear();
        next_output_frame_id_.reset();
        mark_unsynchronized();
    }
}

void VideoTransportHandler::record_pli_sent() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (direction_ == Direction::Receiver) {
        ++stats_.pli_packets;
    }
}

bool VideoTransportHandler::needs_keyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !stats_.synchronized;
}

VideoTransportStats VideoTransportHandler::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::optional<FrameTiming>
VideoTransportHandler::take_completed_frame(uint32_t rtp_timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = completed_timings_.find(rtp_timestamp);
    if (it == completed_timings_.end()) {
        return std::nullopt;
    }
    FrameTiming timing = it->second;
    completed_timings_.erase(it);
    return timing;
}

void VideoTransportHandler::on_keyframe_requested(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    keyframe_requested_callback_ = std::move(callback);
}

void VideoTransportHandler::on_keyframe_acknowledged(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    keyframe_acknowledged_callback_ = std::move(callback);
}

void VideoTransportHandler::on_sync_restored(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    sync_restored_callback_ = std::move(callback);
}

void VideoTransportHandler::on_frame_dropped(
    std::function<void(FrameTiming)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_dropped_callback_ = std::move(callback);
}

void VideoTransportHandler::sender_outgoing(rtc::message_vector &messages) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<rtc::message_ptr> media_packets;
    for (const auto &message : messages) {
        if (is_rtp(message)) {
            media_packets.push_back(message);
        }
    }
    if (media_packets.empty()) {
        return;
    }

    const uint32_t frame_id = next_frame_id_++;
    const uint16_t packet_count = static_cast<uint16_t>(
        std::min<std::size_t>(media_packets.size(),
                              std::numeric_limits<uint16_t>::max()));
    const bool keyframe = next_frame_keyframe_;

    for (uint16_t index = 0; index < packet_count; ++index) {
        const auto &message = media_packets[index];
        add_frame_metadata(
            message,
            FrameMetadata{epoch_, frame_id, index, packet_count, keyframe,
                          next_frame_sender_start_us_});
    }
}

void VideoTransportHandler::sender_incoming(
    const rtc::message_vector &messages,
    const rtc::message_callback &send) {
    (void)send;
    std::function<void()> request_keyframe;
    std::function<void()> keyframe_acknowledged;

    for (const auto &message : messages) {
        if (!message || message->type != rtc::Message::Control ||
            message->size() < sizeof(rtc::RtcpHeader)) {
            continue;
        }

        std::size_t offset = 0;
        while (offset + sizeof(rtc::RtcpHeader) <= message->size()) {
            const auto *header = reinterpret_cast<const rtc::RtcpHeader *>(
                message->data() + offset);
            const std::size_t packet_size = header->lengthInBytes();
            if (packet_size < sizeof(rtc::RtcpHeader) ||
                offset + packet_size > message->size()) {
                break;
            }

            const std::byte *packet = message->data() + offset;
            if (header->payloadType() == kRtcpTransportFeedback &&
                header->reportCount() == kNackFeedbackFormat &&
                packet_size >= offsetof(rtc::RtcpNack, parts)) {
                auto *nack = reinterpret_cast<rtc::RtcpNack *>(
                    const_cast<std::byte *>(packet));
                const unsigned int count = nack->getSeqNoCount();
                for (unsigned int i = 0; i < count; ++i) {
                    for (const uint16_t sequence :
                         nack->parts[i].getSequenceNumbers()) {
                        (void)sequence;
                        std::lock_guard<std::mutex> lock(mutex_);
                        ++stats_.retransmitted_packets;
                    }
                }
            } else if (header->payloadType() == kRtcpPayloadFeedback &&
                       header->reportCount() == kPliFeedbackFormat) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.pli_packets;
                    request_keyframe = keyframe_requested_callback_;
                }
            } else if (header->payloadType() == kRtcpApp &&
                       packet_size >= kKeyframeAckSize &&
                       read_u32(packet + 8) == kKeyframeAckName) {
                std::lock_guard<std::mutex> lock(mutex_);
                keyframe_acknowledged = keyframe_acknowledged_callback_;
            }
            offset += packet_size;
        }
    }

    if (request_keyframe) {
        request_keyframe();
    }
    if (keyframe_acknowledged) {
        keyframe_acknowledged();
    }
}

void VideoTransportHandler::receiver_incoming(
    rtc::message_vector &messages,
    const rtc::message_callback &send) {
    rtc::message_vector control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_send_ = send;
        for (const auto &message : messages) {
            if (!is_rtp(message)) {
                control.push_back(message);
                continue;
            }
            process_receiver_packet(message);
        }

        drain_complete_frames(control, send);
    }
    messages.swap(control);
    receiver_poll(send);
}

void VideoTransportHandler::process_receiver_packet(
    const rtc::message_ptr &message) {
    const auto metadata = read_frame_metadata(message);
    if (!metadata || metadata->packet_count == 0 ||
        metadata->packet_index >= metadata->packet_count) {
        mark_unsynchronized();
        return;
    }

    if (!receiver_epoch_ || *receiver_epoch_ != metadata->epoch) {
        for (const auto &entry : frames_) {
            record_network_drop(entry.second);
        }
        frames_.clear();
        receiver_epoch_ = metadata->epoch;
        next_output_frame_id_ = metadata->frame_id;
        mark_unsynchronized();
    }

    if (!next_output_frame_id_) {
        next_output_frame_id_ = metadata->frame_id;
    }
    if (metadata->frame_id > *next_output_frame_id_) {
        for (uint32_t missing = *next_output_frame_id_;
             missing < metadata->frame_id &&
             frames_.size() < kMaxBufferedFrames;
             ++missing) {
            auto [it, inserted] = frames_.try_emplace(missing);
            if (inserted) {
                it->second.epoch = metadata->epoch;
                it->second.frame_id = missing;
                it->second.first_seen_us = now_us();
            }
        }
    }

    auto &frame = frames_[metadata->frame_id];
    if (frame.first_seen_us == 0) {
        frame.epoch = metadata->epoch;
        frame.frame_id = metadata->frame_id;
        frame.packet_count = metadata->packet_count;
        frame.keyframe = metadata->keyframe;
        frame.sender_start_us = metadata->sender_start_us;
        frame.first_seen_us = now_us();
        frame.packets.resize(metadata->packet_count);
        const auto *header =
            reinterpret_cast<const rtc::RtpHeader *>(message->data());
        frame.rtp_timestamp = header->timestamp();
        frame.first_sequence = static_cast<uint16_t>(
            header->seqNumber() - metadata->packet_index);
    }

    if (frame.packet_count != metadata->packet_count ||
        frame.epoch != metadata->epoch ||
        frame.keyframe != metadata->keyframe) {
        record_network_drop(frame);
        mark_unsynchronized();
        frames_.erase(metadata->frame_id);
        return;
    }

    if (!frame.packets[metadata->packet_index]) {
        frame.packets[metadata->packet_index] = message;
    }
    const auto *header =
        reinterpret_cast<const rtc::RtpHeader *>(message->data());
    frame.marker_seen = frame.marker_seen || header->marker();

    while (frames_.size() > kMaxBufferedFrames) {
        record_network_drop(frames_.begin()->second);
        frames_.erase(frames_.begin());
        mark_unsynchronized();
    }
}

void VideoTransportHandler::drain_complete_frames(
    rtc::message_vector &ready,
    const rtc::message_callback &send) {
    const int64_t at_us = now_us();

    if (!stats_.synchronized) {
        for (auto it = frames_.begin(); it != frames_.end();) {
            auto &frame = it->second;
            if (frame.complete() && frame.keyframe) {
                if (sync_restored_callback_) {
                    sync_restored_callback_();
                }
                for (auto &packet : frame.packets) {
                    ready.push_back(std::move(packet));
                }
                ++stats_.complete_frames;
                stats_.synchronized = true;
                store_completed_timing(frame);
                next_output_frame_id_ = frame.frame_id + 1;
                send_keyframe_ack(frame.epoch, frame.frame_id, send);
                for (auto dropped = frames_.begin(); dropped != it; ++dropped) {
                    record_network_drop(dropped->second);
                }
                frames_.erase(frames_.begin(), std::next(it));
                break;
            }

            if (frame.complete() ||
                at_us - frame.first_seen_us >= recovery_timeout_us_) {
                ++stats_.dropped_frames;
                record_network_drop(frame);
                it = frames_.erase(it);
            } else {
                ++it;
            }
        }
        return;
    }

    while (next_output_frame_id_) {
        const auto it = frames_.find(*next_output_frame_id_);
        if (it == frames_.end()) {
            return;
        }
        auto &frame = it->second;
        if (frame.complete()) {
            store_completed_timing(frame);
            for (auto &packet : frame.packets) {
                ready.push_back(std::move(packet));
            }
            ++stats_.complete_frames;
            ++*next_output_frame_id_;
            frames_.erase(it);
            continue;
        }

        if (at_us - frame.first_seen_us >= recovery_timeout_us_) {
            ++stats_.dropped_frames;
            record_network_drop(frame);
            ++*next_output_frame_id_;
            frames_.erase(it);
            mark_unsynchronized();
        }
        return;
    }
}

void VideoTransportHandler::receiver_poll(
    const rtc::message_callback &send) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t at_us = now_us();

    for (auto &entry : frames_) {
        auto &frame = entry.second;
        if (frame.packet_count == 0 || frame.complete()) {
            continue;
        }
        const bool due =
            (frame.marker_seen ||
             at_us - frame.first_seen_us >= kNackDelayUs) &&
            (frame.last_nack_us == 0 ||
             at_us - frame.last_nack_us >= kNackRetryUs) &&
            at_us - frame.first_seen_us < recovery_timeout_us_;
        if (due) {
            send_nack(frame, send);
            frame.last_nack_us = at_us;
        }
    }

}

void VideoTransportHandler::send_nack(
    const FrameAssembly &frame,
    const rtc::message_callback &send) {
    std::vector<uint16_t> missing;
    for (uint16_t i = 0; i < frame.packet_count; ++i) {
        if (!frame.packets[i]) {
            missing.push_back(static_cast<uint16_t>(frame.first_sequence + i));
        }
    }
    if (missing.empty()) {
        return;
    }

    struct NackGroup {
        uint16_t pid = 0;
        uint16_t blp = 0;
    };
    std::vector<NackGroup> groups;
    for (const uint16_t sequence : missing) {
        if (groups.empty() ||
            static_cast<uint16_t>(sequence - groups.back().pid) > 16U) {
            groups.push_back({sequence, 0});
        } else {
            const uint16_t bit =
                static_cast<uint16_t>(sequence - groups.back().pid - 1U);
            groups.back().blp =
                static_cast<uint16_t>(groups.back().blp | (1U << bit));
        }
    }

    auto message = rtc::make_message(
        rtc::RtcpNack::Size(static_cast<unsigned int>(groups.size())),
        rtc::Message::Control);
    auto *nack = reinterpret_cast<rtc::RtcpNack *>(message->data());
    nack->preparePacket(0x12345678,
                        static_cast<unsigned int>(groups.size()));
    for (std::size_t i = 0; i < groups.size(); ++i) {
        nack->parts[i].setPid(groups[i].pid);
        nack->parts[i].setBlp(groups[i].blp);
    }
    send(message);
    ++stats_.nack_packets;
}

void VideoTransportHandler::send_keyframe_ack(
    uint16_t epoch,
    uint32_t frame_id,
    const rtc::message_callback &send) {
    auto message =
        rtc::make_message(kKeyframeAckSize, rtc::Message::Control);
    std::fill(message->begin(), message->end(), std::byte{0});
    auto *bytes = message->data();
    bytes[0] = std::byte{0x80};
    bytes[1] = static_cast<std::byte>(kRtcpApp);
    const uint16_t length =
        htons(static_cast<uint16_t>(kKeyframeAckSize / 4U - 1U));
    std::memcpy(bytes + 2, &length, sizeof(length));
    const uint32_t name = htonl(kKeyframeAckName);
    std::memcpy(bytes + 8, &name, sizeof(name));
    const uint16_t network_epoch = htons(epoch);
    std::memcpy(bytes + 12, &network_epoch, sizeof(network_epoch));
    const uint32_t network_frame = htonl(frame_id);
    std::memcpy(bytes + 16, &network_frame, sizeof(network_frame));
    send(message);
}

void VideoTransportHandler::mark_unsynchronized() {
    stats_.synchronized = false;
}

void VideoTransportHandler::record_network_drop(
    const FrameAssembly &frame) {
    if (!frame_dropped_callback_) {
        return;
    }
    FrameTiming timing;
    timing.epoch = frame.epoch;
    timing.frame_id = frame.frame_id;
    timing.rtp_timestamp = frame.rtp_timestamp;
    timing.sender_start_us = frame.sender_start_us;
    timing.first_packet_us = frame.first_seen_us;
    timing.synchronized = false;
    frame_dropped_callback_(timing);
}

void VideoTransportHandler::store_completed_timing(
    const FrameAssembly &frame) {
    FrameTiming timing;
    timing.epoch = frame.epoch;
    timing.frame_id = frame.frame_id;
    timing.rtp_timestamp = frame.rtp_timestamp;
    timing.sender_start_us = frame.sender_start_us;
    timing.first_packet_us = frame.first_seen_us;
    timing.frame_complete_us = now_us();
    timing.synchronized = stats_.synchronized;
    completed_timings_[timing.rtp_timestamp] = timing;
    while (completed_timings_.size() > kMaxBufferedFrames) {
        completed_timings_.erase(completed_timings_.begin());
    }
}

void VideoTransportHandler::add_frame_metadata(
    const rtc::message_ptr &message,
    const FrameMetadata &metadata) {
    if (!is_rtp(message)) {
        return;
    }

    const auto *old_header =
        reinterpret_cast<const rtc::RtpHeader *>(message->data());
    const std::size_t fixed_header_size =
        12U + old_header->csrcCount() * 4U;
    if (message->size() < fixed_header_size) {
        return;
    }

    std::size_t old_payload_offset = fixed_header_size;
    std::vector<std::byte> extension_body;
    if (old_header->extension()) {
        if (message->size() <
            fixed_header_size + sizeof(rtc::RtpExtensionHeader)) {
            return;
        }
        const auto *extension =
            reinterpret_cast<const rtc::RtpExtensionHeader *>(
                message->data() + fixed_header_size);
        if (extension->profileSpecificId() != kOneByteExtensionProfile) {
            return;
        }
        const std::size_t extension_size =
            static_cast<std::size_t>(extension->headerLength()) * 4U;
        old_payload_offset += sizeof(rtc::RtpExtensionHeader) + extension_size;
        if (message->size() < old_payload_offset) {
            return;
        }
        const auto *body =
            message->data() + fixed_header_size +
            sizeof(rtc::RtpExtensionHeader);
        extension_body.assign(body, body + extension_size);

        std::size_t used = 0;
        for (std::size_t offset = 0; offset < extension_body.size();) {
            const uint8_t descriptor =
                std::to_integer<uint8_t>(extension_body[offset]);
            if (descriptor == 0) {
                ++offset;
                continue;
            }
            const uint8_t id = descriptor >> 4U;
            const std::size_t value_size = (descriptor & 0x0fU) + 1U;
            if (id == 15 ||
                offset + 1U + value_size > extension_body.size()) {
                break;
            }
            used = offset + 1U + value_size;
            offset = used;
        }
        extension_body.resize(used);
    }

    std::vector<std::byte> value;
    value.reserve(kFrameMetadataSize);
    value.push_back(
        static_cast<std::byte>(0x10U | (metadata.keyframe ? 0x01U : 0U)));
    append_u16(value, metadata.epoch);
    append_u32(value, metadata.frame_id);
    append_u16(value, metadata.packet_index);
    append_u16(value, metadata.packet_count);
    append_u32(value, metadata.sender_start_us);

    extension_body.push_back(static_cast<std::byte>(
        (kFrameInfoExtMapId << 4U) | (kFrameMetadataSize - 1U)));
    extension_body.insert(extension_body.end(), value.begin(), value.end());
    while (extension_body.size() % 4U != 0U) {
        extension_body.push_back(std::byte{0});
    }

    std::vector<std::byte> rewritten;
    rewritten.reserve(message->size() + kFrameMetadataSize + 8U);
    rewritten.insert(rewritten.end(), message->begin(),
                     message->begin() + fixed_header_size);
    rewritten[0] = static_cast<std::byte>(
        std::to_integer<uint8_t>(rewritten[0]) | 0x10U);
    append_u16(rewritten, kOneByteExtensionProfile);
    append_u16(rewritten,
               static_cast<uint16_t>(extension_body.size() / 4U));
    rewritten.insert(rewritten.end(), extension_body.begin(),
                     extension_body.end());
    rewritten.insert(rewritten.end(),
                     message->begin() + old_payload_offset, message->end());
    message->assign(rewritten.begin(), rewritten.end());
}

std::optional<VideoTransportHandler::FrameMetadata>
VideoTransportHandler::read_frame_metadata(
    const rtc::message_ptr &message) const {
    if (!is_rtp(message)) {
        return std::nullopt;
    }
    const auto *header =
        reinterpret_cast<const rtc::RtpHeader *>(message->data());
    if (!header->extension()) {
        return std::nullopt;
    }
    const std::size_t fixed_header_size =
        12U + header->csrcCount() * 4U;
    if (message->size() <
        fixed_header_size + sizeof(rtc::RtpExtensionHeader)) {
        return std::nullopt;
    }
    const auto *extension =
        reinterpret_cast<const rtc::RtpExtensionHeader *>(
            message->data() + fixed_header_size);
    if (extension->profileSpecificId() != kOneByteExtensionProfile) {
        return std::nullopt;
    }
    const std::size_t body_size =
        static_cast<std::size_t>(extension->headerLength()) * 4U;
    const auto *body =
        message->data() + fixed_header_size +
        sizeof(rtc::RtpExtensionHeader);
    if (message->size() <
        fixed_header_size + sizeof(rtc::RtpExtensionHeader) + body_size) {
        return std::nullopt;
    }

    for (std::size_t offset = 0; offset < body_size;) {
        const uint8_t descriptor = std::to_integer<uint8_t>(body[offset]);
        if (descriptor == 0) {
            ++offset;
            continue;
        }
        const uint8_t id = descriptor >> 4U;
        const std::size_t value_size = (descriptor & 0x0fU) + 1U;
        if (id == 15 || offset + 1U + value_size > body_size) {
            break;
        }
        if (id == kFrameInfoExtMapId &&
            value_size == kFrameMetadataSize) {
            const auto *value = body + offset + 1U;
            if ((std::to_integer<uint8_t>(value[0]) >> 4U) != 1U) {
                return std::nullopt;
            }
            FrameMetadata metadata;
            metadata.keyframe =
                (std::to_integer<uint8_t>(value[0]) & 0x01U) != 0;
            metadata.epoch = read_u16(value + 1);
            metadata.frame_id = read_u32(value + 3);
            metadata.packet_index = read_u16(value + 7);
            metadata.packet_count = read_u16(value + 9);
            metadata.sender_start_us = read_u32(value + 11);
            return metadata;
        }
        offset += 1U + value_size;
    }
    return std::nullopt;
}
