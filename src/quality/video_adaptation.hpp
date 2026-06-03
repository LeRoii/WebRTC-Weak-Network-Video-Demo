#pragma once

#include "common/types.hpp"

class VideoAdaptationController {
public:
    VideoProfile update(const NetworkQuality &quality);

private:
    VideoProfile profile_;
};
