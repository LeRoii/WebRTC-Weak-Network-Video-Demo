#include "quality/video_adaptation.hpp"

#include <algorithm>
#include <array>

namespace {
constexpr std::array<VideoProfile, 5> kProfiles{{
    {0, 2000, 30, 1280, 720},
    {1, 1200, 24, 960, 540},
    {2, 600, 15, 640, 360},
    {3, 300, 10, 426, 240},
    {4, 150, 5, 320, 180},
}};

constexpr std::array<double, 5> kRecoveryLossThreshold{{
    0.0, 1.0, 7.0, 25.0, 50.0,
}};
}

VideoAdaptationController::VideoAdaptationController(int max_video_kbps)
    : max_video_kbps_(std::clamp(max_video_kbps, 150, 2000)),
      profile_(profile_for_level(0)) {}

VideoProfile VideoAdaptationController::update(
    const NetworkQuality &quality) {
    const double loss =
        std::max(quality.loss_percent, quality.transport_loss_percent);

    int required_level = 0;
    if (loss > 60.0) {
        required_level = 4;
    } else if (loss > 30.0) {
        required_level = 3;
    } else if (loss > 10.0) {
        required_level = 2;
    } else if (loss > 3.0) {
        required_level = 1;
    }

    if (quality.rtt_ms > 350.0) {
        required_level = std::max(required_level, 3);
    } else if (quality.rtt_ms > 180.0) {
        required_level = std::max(required_level, 2);
    }
    if (quality.jitter_ms > 50.0) {
        required_level = std::max(required_level, 1);
    }

    if (required_level > level_) {
        level_ = required_level;
        recovery_windows_ = 0;
    } else if (level_ > 0 &&
               loss < kRecoveryLossThreshold[level_] &&
               quality.rtt_ms < 150.0 &&
               quality.jitter_ms < 40.0) {
        if (++recovery_windows_ >= 5) {
            --level_;
            recovery_windows_ = 0;
        }
    } else {
        recovery_windows_ = 0;
    }

    profile_ = profile_for_level(level_);
    return profile_;
}

VideoProfile VideoAdaptationController::profile_for_level(int level) const {
    VideoProfile profile =
        kProfiles[static_cast<std::size_t>(std::clamp(level, 0, 4))];
    profile.bitrate_kbps =
        std::min(profile.bitrate_kbps, max_video_kbps_);
    return profile;
}
