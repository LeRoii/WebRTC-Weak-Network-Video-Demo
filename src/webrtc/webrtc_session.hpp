#pragma once

#include "common/types.hpp"
#include "quality/network_quality_estimator.hpp"
#include "quality/video_adaptation.hpp"
#include "signaling/udp_signaling.hpp"
#include "video/video_receiver.hpp"
#include "video/video_sender.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <rtc/rtc.hpp>
#include <string>
#include <thread>

class WebRtcSession {
public:
    WebRtcSession(Role role,
                  Endpoint local,
                  Endpoint peer,
                  std::string video_file,
                  std::string output_file);
    ~WebRtcSession();

    void run();

private:
    void configure_peer_connection();
    void configure_signaling_callbacks();
    void configure_video();
    void handle_signaling_message(const std::string &message);

    void stats_loop();

    static void print_sender_quality(const NetworkQuality &quality,
                                     const VideoProfile &video);
    static void print_receiver_video_stats(const VideoReceiveStatsSnapshot &stats);

    Role role_;
    UdpSignaling signaling_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> video_track_;

    NetworkQualityEstimator estimator_;
    VideoAdaptationController adaptation_;
    VideoSender video_sender_;
    VideoReceiver video_receiver_;

    std::atomic<bool> stopping_{false};
};
