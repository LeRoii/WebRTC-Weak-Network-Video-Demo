#include "video/video_sender.hpp"

#include "quality/media_statistics_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/pacinghandler.hpp>
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
    constexpr uint16_t kMaxFragmentSize = 1100;

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
    auto transport_handler =
        std::make_shared<VideoTransportHandler>(
            VideoTransportHandler::Direction::Sender);
    auto pacing_handler = std::make_shared<rtc::PacingHandler>(
        2'300'000.0, std::chrono::milliseconds(5), 8U * 1024U * 1024U);
    auto nack_responder =
        std::make_shared<rtc::RtcpNackResponder>(4096);

    transport_handler->on_keyframe_requested([this]() {
        recovery_mode_.store(true);
        force_keyframe_.store(true);
    });
    transport_handler->on_keyframe_acknowledged([this]() {
        recovery_mode_.store(false);
    });

    if (stats_handler) {
        packetizer->addToChain(stats_handler);
    }
    packetizer->addToChain(transport_handler);
    packetizer->addToChain(sr_reporter);
    packetizer->addToChain(nack_responder);
    packetizer->addToChain(pacing_handler);
    track->setMediaHandler(packetizer);

    std::lock_guard<std::mutex> lock(mutex_);
    track_ = std::move(track);
    media_handler_ = std::move(packetizer);
    stats_handler_ = std::move(stats_handler);
    transport_handler_ = std::move(transport_handler);
    sr_reporter_ = std::move(sr_reporter);
    nack_responder_ = std::move(nack_responder);
    pacing_handler_ = std::move(pacing_handler);
}

void VideoSender::update_profile(VideoProfile profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (profile_ != profile) {
        force_keyframe_.store(true);
    }
    profile_ = profile;
}

void VideoSender::update_network_quality(NetworkQuality quality) {
    std::shared_ptr<rtc::PacingHandler> pacing;
    VideoProfile profile;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quality_ = quality;
        pacing = pacing_handler_;
        profile = profile_;
    }
    if (!pacing) {
        return;
    }
    const double loss =
        std::max(quality.loss_percent, quality.transport_loss_percent);
    const double survival = std::max(0.20, 1.0 - loss / 100.0);
    const double pacing_bps = std::clamp(
        static_cast<double>(profile.bitrate_kbps) * 1000.0 *
            1.15 / survival,
        static_cast<double>(profile.bitrate_kbps) * 1150.0,
        8'000'000.0);
    pacing->setBitrate(pacing_bps);
}

void VideoSender::set_video_file(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_reader_ = std::make_unique<VideoFileReader>(std::move(path));
    file_reader_->on_source_frame(
        [this](const AVFrame *frame) { source_recorder_.submit(frame); });
}

void VideoSender::set_demo_source_record_file(const std::string &path) {
    if (!path.empty()) {
        source_recorder_.start(path);
    }
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
    source_recorder_.stop();
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

        const int64_t at_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        if (recovery_mode_.load() &&
            last_recovery_frame_us_ != 0 &&
            at_us - last_recovery_frame_us_ < 250'000) {
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

            const bool force_keyframe =
                force_keyframe_.exchange(false) || recovery_mode_.load();
            if (!file_reader_->next_frame(frame, profile, force_keyframe)) {
                file_reader_->reset();
                force_keyframe_.store(true);
                continue;
            }
        }

        std::shared_ptr<VideoTransportHandler> transport;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transport = transport_handler_;
        }
        if (transport) {
            transport->set_outgoing_frame(
                frame.epoch, frame.keyframe, frame.sender_start_us);
        }

        const std::size_t frame_bytes = frame.data.size();
        if (!send_frame(std::move(frame.data), timestamp_, frame.keyframe)) {
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
        if (recovery_mode_.load()) {
            last_recovery_frame_us_ = at_us;
        }
        timestamp_ += frame.duration_90khz;
        const auto frame_interval =
            std::chrono::microseconds(frame.duration_90khz * 1'000'000ULL / 90000);
        std::this_thread::sleep_for(frame_interval);
    }
}

bool VideoSender::send_frame(rtc::binary frame,
                             uint32_t timestamp,
                             bool keyframe) {
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
        info.isKeyFrame = keyframe;
        track->sendFrame(std::move(frame), info);
        return true;
    } catch (const std::exception &error) {
        std::cerr << "video_send_failed=" << error.what() << std::endl;
        return false;
    }
}

VideoTransportStats VideoSender::transport_snapshot() const {
    std::shared_ptr<VideoTransportHandler> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_handler_;
    }
    return transport ? transport->snapshot() : VideoTransportStats{};
}
