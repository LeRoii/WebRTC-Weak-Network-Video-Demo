#pragma once

#include "quality/network_quality_estimator.hpp"
#include "video/h264_decoder.hpp"
#include "video/video_renderer.hpp"
#include "video/video_stats.hpp"
#include "video/video_transport_handler.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <rtc/mediahandler.hpp>
#include <rtc/rtc.hpp>
#include <string>
#include <thread>

class VideoReceiver {
public:
    VideoReceiver();
    ~VideoReceiver();

    void start_display();
    void set_output_file(const std::string &path);
    void set_quality_estimator(NetworkQualityEstimator *estimator);
    void set_recovery_timeout_ms(int timeout_ms);
    void attach_track(std::shared_ptr<rtc::Track> track);
    VideoReceiveStatsSnapshot snapshot() const;

private:
    void recovery_loop();

    mutable VideoReceiveStats stats_;
    std::mutex output_mutex_;
    std::ofstream output_;
    bool output_started_ = false;
    std::shared_ptr<rtc::MediaHandler> media_handler_;
    std::shared_ptr<rtc::MediaHandler> stats_handler_;
    std::shared_ptr<VideoTransportHandler> transport_handler_;
    std::shared_ptr<RtxReceivingHandler> rtx_handler_;
    std::shared_ptr<rtc::Track> track_;
    NetworkQualityEstimator *estimator_ = nullptr;
    int recovery_timeout_ms_ = 1000;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> track_open_{false};
    std::atomic<bool> reset_decoder_{true};
    std::thread recovery_thread_;
    VideoRenderer renderer_;
    H264Decoder decoder_;
};
