#include "video/video_receiver.hpp"

#include "quality/media_statistics_handler.hpp"

#include <iostream>
#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <stdexcept>

VideoReceiver::VideoReceiver() : decoder_(renderer_) {}

VideoReceiver::~VideoReceiver() {
    renderer_.stop();
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

void VideoReceiver::set_quality_estimator(NetworkQualityEstimator *estimator) {
    estimator_ = estimator;
}

void VideoReceiver::attach_track(std::shared_ptr<rtc::Track> track) {
    auto stats_handler = estimator_
                             ? std::make_shared<MediaStatisticsHandler>(
                                   *estimator_, MediaStatisticsHandler::Direction::Receiver)
                             : nullptr;
    auto rtcp_session = std::make_shared<rtc::RtcpReceivingSession>();
    auto depacketizer =
        std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence);
    if (stats_handler) {
        depacketizer->addToChain(rtcp_session);
        rtcp_session->addToChain(stats_handler);
        track->setMediaHandler(depacketizer);
        media_handler_ = std::move(depacketizer);
        stats_handler_ = std::move(stats_handler);
    } else {
        depacketizer->addToChain(rtcp_session);
        track->setMediaHandler(depacketizer);
        media_handler_ = std::move(depacketizer);
    }

    track->onOpen([]() { std::cerr << "video_track_open=1" << std::endl; });
    track->onClosed([]() { std::cerr << "video_track_closed=1" << std::endl; });
    track->onFrame([this](rtc::binary frame, rtc::FrameInfo) {
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            if (output_) {
                output_.write(reinterpret_cast<const char *>(frame.data()),
                              static_cast<std::streamsize>(frame.size()));
                output_.flush();
            }
        }

        decoder_.decode(frame.data(), frame.size());
        stats_.on_encoded_frame(frame.size());
    });
}

VideoReceiveStatsSnapshot VideoReceiver::snapshot() const {
    return stats_.snapshot();
}
