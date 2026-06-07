#pragma once

#include "common/types.hpp"

class VideoAdaptationController {
public:
    explicit VideoAdaptationController(int max_video_kbps = 2000);
    VideoProfile update(const NetworkQuality &quality);

private:
    VideoProfile profile_for_level(int level) const;

    int max_video_kbps_ = 2000;
    int level_ = 0;
    int recovery_windows_ = 0;
    VideoProfile profile_;
};
