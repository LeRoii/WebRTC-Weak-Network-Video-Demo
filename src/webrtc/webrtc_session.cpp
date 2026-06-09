#include "webrtc/webrtc_session.hpp"

#include "common/utils.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::chrono::seconds kStatsInterval(1);
constexpr std::chrono::seconds kRunForeverCheckInterval(1);
constexpr int kVideoPayloadType = 102;
constexpr int kVideoRtxPayloadType = 103;
constexpr uint32_t kVideoSsrc = 0x12345678;
constexpr uint32_t kVideoRtxSsrc = 0x12345679;
constexpr int kTwccExtMapId = 3;
constexpr int kFrameInfoExtMapId = 4;
constexpr const char *kTwccExtUri =
    "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
constexpr const char *kFrameInfoExtUri =
    "urn:webrtc-net-quality-probe:rtp-hdrext:frame-info";
}

WebRtcSession::WebRtcSession(Role role,
                             Endpoint local,
                             Endpoint peer,
                             std::string video_file,
                             std::string output_file,
                             std::string demo_record_file,
                             std::string demo_source_record_file,
                             std::string latency_csv,
                             int max_video_kbps,
                             int recovery_timeout_ms)
    : role_(role),
      signaling_(std::move(local), std::move(peer)),
      adaptation_(max_video_kbps) {
    rtc::InitLogger(rtc::LogLevel::Warning);

    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    pc_ = std::make_shared<rtc::PeerConnection>(config);

    configure_signaling_callbacks();
    configure_peer_connection();

    if (role_ == Role::Sender) {
        video_sender_.set_quality_estimator(&estimator_);
        video_sender_.set_video_file(std::move(video_file));
        video_sender_.set_demo_source_record_file(demo_source_record_file);
    } else {
        video_receiver_.set_quality_estimator(&estimator_);
        video_receiver_.set_recovery_timeout_ms(recovery_timeout_ms);
        video_receiver_.set_latency_csv(latency_csv);
        video_receiver_.start_display();
        video_receiver_.set_output_file(output_file);
        video_receiver_.set_demo_record_file(demo_record_file);
    }

    configure_video();
}

WebRtcSession::~WebRtcSession() {
    stopping_.store(true);
    video_sender_.stop();
    signaling_.stop();
}

void WebRtcSession::run() {
    signaling_.start([this](std::string message) { handle_signaling_message(message); });

    if (role_ == Role::Sender) {
        pc_->setLocalDescription();
    }

    std::thread stats_thread([this] { stats_loop(); });

    if (role_ == Role::Sender) {
        video_sender_.start();
    }

    while (!stopping_.load() && !g_stop_requested) {
        std::this_thread::sleep_for(kRunForeverCheckInterval);
    }

    stopping_.store(true);
    video_sender_.stop();
    signaling_.stop();

    if (stats_thread.joinable()) {
        stats_thread.join();
    }
}

void WebRtcSession::configure_peer_connection() {
    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        std::cerr << "peer_state=" << static_cast<int>(state) << std::endl;
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            stopping_.store(true);
        }
    });

    pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
        std::cerr << "remote_video_track=1" << std::endl;
        video_receiver_.attach_track(std::move(track));
    });
}

void WebRtcSession::configure_signaling_callbacks() {
    pc_->onLocalDescription([this](rtc::Description description) {
        const std::string sdp = static_cast<std::string>(description);
        const std::string type = description.typeString();
        signaling_.send("DESC|" + type + "|" + hex_encode(sdp));
    });

    pc_->onLocalCandidate([this](rtc::Candidate candidate) {
        const std::string candidate_text = static_cast<std::string>(candidate);
        const std::string mid = candidate.mid();
        signaling_.send("CAND|" + hex_encode(mid) + "|" + hex_encode(candidate_text));
    });
}

void WebRtcSession::configure_video() {
    rtc::Description::Video media("video-stream",
                                  role_ == Role::Sender
                                      ? rtc::Description::Direction::SendOnly
                                      : rtc::Description::Direction::RecvOnly);
    media.addH264Codec(kVideoPayloadType);
    media.addRtxCodec(kVideoRtxPayloadType, kVideoPayloadType, 90000);
    if (auto *rtp_map = media.rtpMap(kVideoPayloadType)) {
        rtp_map->addFeedback("transport-cc");
        rtp_map->addFeedback("nack");
        rtp_map->addFeedback("nack pli");
    }
    media.addExtMap(rtc::Description::Entry::ExtMap(kTwccExtMapId, kTwccExtUri));
    media.addExtMap(
        rtc::Description::Entry::ExtMap(kFrameInfoExtMapId, kFrameInfoExtUri));
    if (role_ == Role::Sender) {
        media.addSSRC(kVideoSsrc, "video-stream", "stream1", "video-stream");
        media.addRtxSSRC(kVideoSsrc, kVideoRtxSsrc, "video-stream");
    }
    video_track_ = pc_->addTrack(media);

    if (role_ == Role::Sender) {
        video_sender_.set_track(video_track_);
    } else {
        video_receiver_.attach_track(video_track_);
    }
}

void WebRtcSession::handle_signaling_message(const std::string &message) {
    const auto parts = split(message, '|');
    if (parts.empty()) {
        return;
    }

    try {
        if (parts[0] == "DESC" && parts.size() >= 3) {
            const std::string type = parts[1];
            const std::string sdp = hex_decode(parts[2]);
            pc_->setRemoteDescription(rtc::Description(sdp, type));
            if (role_ == Role::Receiver && type == "offer") {
                pc_->setLocalDescription();
            }
        } else if (parts[0] == "CAND" && parts.size() >= 3) {
            const std::string mid = hex_decode(parts[1]);
            const std::string candidate = hex_decode(parts[2]);
            pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
        }
    } catch (const std::exception &error) {
        std::cerr << "signaling_message_failed=" << error.what() << std::endl;
    }
}

void WebRtcSession::stats_loop() {
    while (!stopping_.load()) {
        std::this_thread::sleep_for(kStatsInterval);
        const auto quality = estimator_.snapshot();

        if (role_ == Role::Sender) {
            const auto video = adaptation_.update(quality);
            video_sender_.update_profile(video);
            video_sender_.update_network_quality(quality);
            print_sender_quality(quality, video,
                                 video_sender_.transport_snapshot());
        } else {
            print_receiver_video_stats(video_receiver_.snapshot());
        }
    }
}

void WebRtcSession::print_sender_quality(const NetworkQuality &quality,
                                         const VideoProfile &video,
                                         const VideoTransportStats &transport) {
    std::cout << std::fixed << std::setprecision(2)
              << "rtt=" << quality.rtt_ms << "ms "
              << "media_loss=" << quality.loss_percent << "% "
              << "transport_loss=" << quality.transport_loss_percent << "% "
              << "jitter=" << quality.jitter_ms << "ms "
              << "rtp_send=" << quality.send_kbps << "kbps "
              << "rtp_recv=" << quality.receive_kbps << "kbps "
              << "rtcp=" << quality.ack_kbps << "kbps "
              << "retransmit=" << transport.retransmitted_packets << " "
              << "pli=" << transport.pli_packets << " "
              << "video_target=" << video.bitrate_kbps << "kbps/"
              << video.fps << "fps/"
              << video.width << "x" << video.height << std::endl;
}

void WebRtcSession::print_receiver_video_stats(const VideoReceiveStatsSnapshot &stats) {
    std::cout << std::fixed << std::setprecision(2)
              << "video_recv=" << stats.bitrate_kbps << "kbps "
              << "video_frames=" << stats.frames << " "
              << "dropped_frames=" << stats.dropped_frames << " "
              << "nack=" << stats.nack_packets << " "
              << "retransmit=" << stats.retransmitted_packets << " "
              << "pli=" << stats.pli_packets << " "
              << "decoder_errors=" << stats.decoder_errors << " "
              << "sync=" << (stats.synchronized ? 1 : 0) << std::endl;
}
