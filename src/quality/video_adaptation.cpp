#include "quality/video_adaptation.hpp"

#include <algorithm>

VideoProfile VideoAdaptationController::update(const NetworkQuality &quality) {
    const bool severe = quality.loss_percent > 10.0 || quality.rtt_ms > 350.0;
    const bool degraded = quality.loss_percent > 4.0 || quality.rtt_ms > 180.0 ||
                          quality.jitter_ms > 50.0;
    const bool good = quality.loss_percent < 1.0 && quality.rtt_ms < 80.0 &&
                      quality.jitter_ms < 20.0;

    if (severe) {
        profile_.bitrate_kbps = std::max(250, profile_.bitrate_kbps * 65 / 100);
        profile_.fps = std::max(10, profile_.fps - 5);
        if (profile_.bitrate_kbps < 700) {
            profile_.width = 640;
            profile_.height = 360;
        }
    } else if (degraded) {
        profile_.bitrate_kbps = std::max(400, profile_.bitrate_kbps * 80 / 100);
        profile_.fps = std::max(15, profile_.fps - 3);
    } else if (good) {
        profile_.bitrate_kbps = std::min(4000, profile_.bitrate_kbps + 150);
        profile_.fps = std::min(30, profile_.fps + 1);
        if (profile_.bitrate_kbps > 1800) {
            profile_.width = 1280;
            profile_.height = 720;
        }
    }

    return profile_;
}
