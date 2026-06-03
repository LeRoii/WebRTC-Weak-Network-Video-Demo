#pragma once

#include <cstdint>
#include <string>

enum class Role {
    Sender,
    Receiver,
};

struct Endpoint {
    std::string ip;
    uint16_t port = 0;
};

struct NetworkQuality {
    double rtt_ms = 0.0;
    double loss_percent = 0.0;
    double jitter_ms = 0.0;
    double transport_loss_percent = 0.0;
    double send_kbps = 0.0;
    double receive_kbps = 0.0;
    double ack_kbps = 0.0;
};

struct VideoProfile {
    int bitrate_kbps = 2500;
    int fps = 30;
    int width = 1280;
    int height = 720;
};
