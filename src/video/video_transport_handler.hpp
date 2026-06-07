#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <rtc/mediahandler.hpp>
#include <rtc/rtc.hpp>
#include <vector>

struct VideoTransportStats {
    uint64_t complete_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t nack_packets = 0;
    uint64_t retransmitted_packets = 0;
    uint64_t pli_packets = 0;
    bool synchronized = false;
};

class RtxReceivingHandler final : public rtc::MediaHandler {
public:
    RtxReceivingHandler(uint8_t primary_payload_type,
                        uint8_t rtx_payload_type,
                        uint32_t primary_ssrc,
                        uint32_t rtx_ssrc);

    void incoming(rtc::message_vector &messages,
                  const rtc::message_callback &send) override;

    uint64_t retransmitted_packets() const;

private:
    const uint8_t primary_payload_type_;
    const uint8_t rtx_payload_type_;
    const uint32_t primary_ssrc_;
    const uint32_t rtx_ssrc_;
    std::atomic<uint64_t> retransmitted_packets_{0};
};

class VideoTransportHandler final : public rtc::MediaHandler {
public:
    enum class Direction {
        Sender,
        Receiver,
    };

    explicit VideoTransportHandler(Direction direction,
                                   int recovery_timeout_ms = 1000);

    void incoming(rtc::message_vector &messages,
                  const rtc::message_callback &send) override;
    void outgoing(rtc::message_vector &messages,
                  const rtc::message_callback &send) override;

    void set_outgoing_frame(uint16_t epoch, bool keyframe);
    void poll();
    void invalidate_sync();
    void record_pli_sent();
    bool needs_keyframe() const;
    VideoTransportStats snapshot() const;

    void on_keyframe_requested(std::function<void()> callback);
    void on_keyframe_acknowledged(std::function<void()> callback);
    void on_sync_restored(std::function<void()> callback);

private:
    struct FrameMetadata {
        uint16_t epoch = 0;
        uint32_t frame_id = 0;
        uint16_t packet_index = 0;
        uint16_t packet_count = 0;
        bool keyframe = false;
    };

    struct FrameAssembly {
        uint16_t epoch = 0;
        uint32_t frame_id = 0;
        uint16_t packet_count = 0;
        uint16_t first_sequence = 0;
        bool keyframe = false;
        bool marker_seen = false;
        int64_t first_seen_us = 0;
        int64_t last_nack_us = 0;
        std::vector<rtc::message_ptr> packets;

        bool complete() const;
    };

    void sender_incoming(const rtc::message_vector &messages,
                         const rtc::message_callback &send);
    void sender_outgoing(rtc::message_vector &messages);
    void receiver_incoming(rtc::message_vector &messages,
                           const rtc::message_callback &send);
    void receiver_poll(const rtc::message_callback &send);

    void add_frame_metadata(const rtc::message_ptr &message,
                            const FrameMetadata &metadata);
    std::optional<FrameMetadata> read_frame_metadata(
        const rtc::message_ptr &message) const;

    void process_receiver_packet(const rtc::message_ptr &message);
    void drain_complete_frames(rtc::message_vector &ready,
                               const rtc::message_callback &send);
    void send_nack(const FrameAssembly &frame,
                   const rtc::message_callback &send);
    void send_keyframe_ack(uint16_t epoch,
                           uint32_t frame_id,
                           const rtc::message_callback &send);
    void mark_unsynchronized();

    Direction direction_;
    const int64_t recovery_timeout_us_;

    mutable std::mutex mutex_;
    uint16_t epoch_ = 1;
    bool next_frame_keyframe_ = false;
    uint32_t next_frame_id_ = 1;
    std::optional<uint16_t> receiver_epoch_;
    std::optional<uint32_t> next_output_frame_id_;
    std::map<uint32_t, FrameAssembly> frames_;
    rtc::message_callback receiver_send_;

    std::function<void()> keyframe_requested_callback_;
    std::function<void()> keyframe_acknowledged_callback_;
    std::function<void()> sync_restored_callback_;

    VideoTransportStats stats_;
};
