#include "video/video_sender.hpp"

#include "quality/media_statistics_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>

void VideoSender::set_quality_estimator(NetworkQualityEstimator *estimator) {
    std::lock_guard<std::mutex> lock(mutex_);
    estimator_ = estimator;
}

void VideoSender::set_track(std::shared_ptr<rtc::Track> track) {
    std::cerr << "video_sender_track_configured=1" << std::endl;

    track->onOpen([this]() {
        std::cerr << "video_track_open=1" << std::endl;
        track_open_.store(true);
    });
    track->onClosed([this]() {
        std::cerr << "video_track_closed=1" << std::endl;
        track_open_.store(false);
        stopping_.store(true);
    });

    constexpr int kPayloadType = 102;
    constexpr int kClockRate = 90000;
    constexpr uint16_t kMaxFragmentSize = 1200;

    auto rtp_config =
        std::make_shared<rtc::RtpPacketizationConfig>(ssrc_, "video-stream",
                                                      kPayloadType, kClockRate);
    rtp_config->mid = track->description().mid();

    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::LongStartSequence, rtp_config, kMaxFragmentSize);
    auto stats_handler = estimator_
                             ? std::make_shared<MediaStatisticsHandler>(
                                   *estimator_, MediaStatisticsHandler::Direction::Sender)
                             : nullptr;
    auto sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
    auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();

    if (stats_handler) {
        packetizer->addToChain(stats_handler);
    }
    packetizer->addToChain(sr_reporter);
    packetizer->addToChain(nack_responder);
    track->setMediaHandler(packetizer);

    std::lock_guard<std::mutex> lock(mutex_);
    track_ = std::move(track);
    media_handler_ = std::move(packetizer);
    stats_handler_ = std::move(stats_handler);
    sr_reporter_ = std::move(sr_reporter);
    nack_responder_ = std::move(nack_responder);
}

void VideoSender::update_profile(VideoProfile profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    profile_ = profile;
}

void VideoSender::set_video_file(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_reader_ = std::make_unique<VideoFileReader>(std::move(path));
}

void VideoSender::start() {
    stopping_.store(false);
    std::cerr << "video_sender_start=1" << std::endl;
    thread_ = std::thread([this] { send_loop(); });
}

void VideoSender::stop() {
    stopping_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void VideoSender::send_loop() {
    while (!stopping_.load()) {
        std::shared_ptr<rtc::Track> track;
        VideoProfile profile;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            track = track_;
            profile = profile_;
        }

        if (!track || !track_open_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        EncodedVideoFrame frame;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!file_reader_) {
                std::cerr << "video_send_failed=no input file configured" << std::endl;
                stopping_.store(true);
                break;
            }

            if (!file_reader_->next_frame(frame)) {
                file_reader_->reset();
                continue;
            }
        }

        const std::size_t frame_bytes = frame.data.size();
        if (!send_frame(std::move(frame.data), timestamp_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (frame_index_ == 0 || frame_index_ % 30 == 0) {
            std::cerr << "video_frame_sent index=" << frame_index_
                      << " timestamp=" << timestamp_
                      << " bytes=" << frame_bytes
                      << " target_kbps=" << profile.bitrate_kbps << std::endl;
        }

        ++frame_index_;
        timestamp_ += frame.duration_90khz;
        const auto frame_interval =
            std::chrono::microseconds(frame.duration_90khz * 1'000'000ULL / 90000);
        std::this_thread::sleep_for(frame_interval);
    }
}

bool VideoSender::send_frame(rtc::binary frame, uint32_t timestamp) {
    std::shared_ptr<rtc::Track> track;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        track = track_;
    }

    if (!track) {
        return false;
    }

    try {
        rtc::FrameInfo info(timestamp);
        track->sendFrame(std::move(frame), info);
        return true;
    } catch (const std::exception &error) {
        std::cerr << "video_send_failed=" << error.what() << std::endl;
        return false;
    }
}
