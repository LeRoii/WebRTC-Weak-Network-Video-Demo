#pragma once

#include "common/types.hpp"
#include "quality/network_quality_estimator.hpp"
#include "video/video_file_reader.hpp"
#include "video/video_transport_handler.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <rtc/mediahandler.hpp>
#include <rtc/pacinghandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtc.hpp>
#include <thread>

class VideoSender {
public:
    void set_quality_estimator(NetworkQualityEstimator *estimator);
    void set_track(std::shared_ptr<rtc::Track> track);
    void set_video_file(std::string path);
    void update_profile(VideoProfile profile);
    void update_network_quality(NetworkQuality quality);
    void start();
    void stop();
    VideoTransportStats transport_snapshot() const;

private:
    void send_loop();
    bool send_frame(rtc::binary frame, uint32_t timestamp, bool keyframe);

    mutable std::mutex mutex_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::MediaHandler> media_handler_;
    std::shared_ptr<rtc::MediaHandler> stats_handler_;
    std::shared_ptr<VideoTransportHandler> transport_handler_;
    std::shared_ptr<rtc::RtcpSrReporter> sr_reporter_;
    std::shared_ptr<rtc::RtcpNackResponder> nack_responder_;
    std::shared_ptr<rtc::PacingHandler> pacing_handler_;
    NetworkQualityEstimator *estimator_ = nullptr;
    std::unique_ptr<VideoFileReader> file_reader_;
    VideoProfile profile_;
    NetworkQuality quality_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> track_open_{false};
    std::atomic<bool> force_keyframe_{true};
    std::atomic<bool> recovery_mode_{false};
    std::thread thread_;
    uint32_t timestamp_ = 1;
    uint64_t frame_index_ = 0;
    uint32_t ssrc_ = 0x12345678;
    int64_t last_recovery_frame_us_ = 0;
};
