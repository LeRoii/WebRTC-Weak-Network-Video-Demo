#pragma once

#include "quality/network_quality_estimator.hpp"
#include "video/h264_decoder.hpp"
#include "video/video_renderer.hpp"
#include "video/video_stats.hpp"

#include <fstream>
#include <memory>
#include <mutex>
#include <rtc/mediahandler.hpp>
#include <rtc/rtc.hpp>
#include <string>

class VideoReceiver {
public:
    VideoReceiver();
    ~VideoReceiver();

    void start_display();
    void set_output_file(const std::string &path);
    void set_quality_estimator(NetworkQualityEstimator *estimator);
    void attach_track(std::shared_ptr<rtc::Track> track);
    VideoReceiveStatsSnapshot snapshot() const;

private:
    VideoReceiveStats stats_;
    std::mutex output_mutex_;
    std::ofstream output_;
    std::shared_ptr<rtc::MediaHandler> media_handler_;
    std::shared_ptr<rtc::MediaHandler> stats_handler_;
    NetworkQualityEstimator *estimator_ = nullptr;
    VideoRenderer renderer_;
    H264Decoder decoder_;
};
