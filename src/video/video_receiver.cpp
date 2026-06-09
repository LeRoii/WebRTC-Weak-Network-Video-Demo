#include "video/video_receiver.hpp"

#include "quality/media_statistics_handler.hpp"

#include <iostream>
#include <chrono>
#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <stdexcept>

namespace {
constexpr uint8_t kVideoPayloadType = 102;
constexpr uint8_t kVideoRtxPayloadType = 103;
constexpr uint32_t kVideoSsrc = 0x12345678;
constexpr uint32_t kVideoRtxSsrc = 0x12345679;

bool contains_h264_idr(const rtc::binary &frame) {
    for (std::size_t i = 0; i + 4 < frame.size(); ++i) {
        std::size_t header = 0;
        if (frame[i] == std::byte{0} &&
            frame[i + 1] == std::byte{0} &&
            frame[i + 2] == std::byte{1}) {
            header = i + 3;
        } else if (i + 5 < frame.size() &&
                   frame[i] == std::byte{0} &&
                   frame[i + 1] == std::byte{0} &&
                   frame[i + 2] == std::byte{0} &&
                   frame[i + 3] == std::byte{1}) {
            header = i + 4;
        }
        if (header != 0 &&
            (std::to_integer<uint8_t>(frame[header]) & 0x1fU) == 5U) {
            return true;
        }
    }
    return false;
}
}

VideoReceiver::VideoReceiver() : decoder_(renderer_) {
    renderer_.on_present(
        [this](const AVFrame *frame, FrameTiming timing) {
            latency_csv_.record(timing, FrameTerminalStatus::Presented);
            demo_recorder_.submit(frame);
        });
    renderer_.on_drop([this](FrameTiming timing) {
        latency_csv_.record(timing, FrameTerminalStatus::RendererDropped);
    });
}

VideoReceiver::~VideoReceiver() {
    stopping_.store(true);
    if (recovery_thread_.joinable()) {
        recovery_thread_.join();
    }
    demo_recorder_.stop();
    renderer_.stop();
    latency_csv_.close();
}

void VideoReceiver::start_display() {
    renderer_.start();
}

void VideoReceiver::set_output_file(const std::string &path) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_.open(path, std::ios::binary | std::ios::trunc);
    if (!output_) {
        throw std::runtime_error("cannot open receiver output file: " + path);
    }
}

void VideoReceiver::set_demo_record_file(const std::string &path) {
    if (!path.empty()) {
        demo_recorder_.start(path);
    }
}

void VideoReceiver::set_latency_csv(const std::string &path) {
    if (!path.empty()) {
        latency_csv_.open(path);
    }
}

void VideoReceiver::set_quality_estimator(NetworkQualityEstimator *estimator) {
    estimator_ = estimator;
}

void VideoReceiver::set_recovery_timeout_ms(int timeout_ms) {
    recovery_timeout_ms_ = timeout_ms;
}

void VideoReceiver::attach_track(std::shared_ptr<rtc::Track> track) {
    auto stats_handler = estimator_
                             ? std::make_shared<MediaStatisticsHandler>(
                                   *estimator_, MediaStatisticsHandler::Direction::Receiver)
                             : nullptr;
    auto rtcp_session = std::make_shared<rtc::RtcpReceivingSession>();
    auto depacketizer =
        std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
    auto transport_handler =
        std::make_shared<VideoTransportHandler>(
            VideoTransportHandler::Direction::Receiver,
            recovery_timeout_ms_);
    auto rtx_handler = std::make_shared<RtxReceivingHandler>(
        kVideoPayloadType, kVideoRtxPayloadType, kVideoSsrc, kVideoRtxSsrc);
    transport_handler->on_sync_restored([this]() {
        reset_decoder_.store(true);
    });
    transport_handler->on_frame_dropped([this](FrameTiming timing) {
        latency_csv_.record(timing, FrameTerminalStatus::NetworkDropped);
    });

    depacketizer->addToChain(transport_handler);
    if (stats_handler) {
        transport_handler->addToChain(stats_handler);
        stats_handler->addToChain(rtcp_session);
    } else {
        transport_handler->addToChain(rtcp_session);
    }
    rtcp_session->addToChain(rtx_handler);
    track->setMediaHandler(depacketizer);
    media_handler_ = std::move(depacketizer);
    stats_handler_ = std::move(stats_handler);
    transport_handler_ = std::move(transport_handler);
    rtx_handler_ = std::move(rtx_handler);
    track_ = track;

    track->onOpen([this]() {
        std::cerr << "video_track_open=1" << std::endl;
        track_open_.store(true);
    });
    track->onClosed([this]() {
        std::cerr << "video_track_closed=1" << std::endl;
        track_open_.store(false);
    });
    track->onFrame([this](rtc::binary frame, rtc::FrameInfo info) {
        const bool keyframe = contains_h264_idr(frame);
        auto timing = transport_handler_
                          ? transport_handler_->take_completed_frame(info.timestamp)
                          : std::nullopt;
        if (!timing) {
            std::cerr << "frame_timing_missing rtp_timestamp="
                      << info.timestamp << std::endl;
            timing = FrameTiming{};
            timing->rtp_timestamp = info.timestamp;
        }
        if (reset_decoder_.exchange(false)) {
            decoder_.reset();
        }
        if (!decoder_.decode(frame.data(), frame.size(), *timing)) {
            stats_.on_decoder_error();
            latency_csv_.record(*timing, FrameTerminalStatus::DecoderError);
            if (transport_handler_) {
                transport_handler_->invalidate_sync();
            }
            reset_decoder_.store(true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            if (!output_started_ && keyframe) {
                output_started_ = true;
            }
            if (output_ && output_started_) {
                output_.write(reinterpret_cast<const char *>(frame.data()),
                              static_cast<std::streamsize>(frame.size()));
                output_.flush();
            }
        }

        stats_.on_encoded_frame(frame.size());
    });

    if (!recovery_thread_.joinable()) {
        stopping_.store(false);
        recovery_thread_ = std::thread([this] { recovery_loop(); });
    }
}

VideoReceiveStatsSnapshot VideoReceiver::snapshot() const {
    if (transport_handler_) {
        const auto transport = transport_handler_->snapshot();
        stats_.set_transport(
            transport.dropped_frames,
            transport.nack_packets,
            rtx_handler_ ? rtx_handler_->retransmitted_packets() : 0,
            transport.pli_packets,
            transport.synchronized);
    }
    return stats_.snapshot();
}

void VideoReceiver::recovery_loop() {
    int64_t last_pli_us = 0;
    while (!stopping_.load()) {
        auto transport = transport_handler_;
        auto track = track_;
        if (transport) {
            transport->poll();
        }
        const int64_t at_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (transport && track && track_open_.load() &&
            transport->needs_keyframe() &&
            at_us - last_pli_us >= 250'000) {
            try {
                track->requestKeyframe();
                transport->record_pli_sent();
            } catch (const std::exception &error) {
                std::cerr << "keyframe_request_failed="
                          << error.what() << std::endl;
            }
            last_pli_us = at_us;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
