#pragma once

#include "common/types.hpp"

#include <atomic>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <thread>

class UdpSignaling {
public:
    UdpSignaling(Endpoint local, Endpoint peer);
    ~UdpSignaling();

    void start(std::function<void(std::string)> on_message);
    void stop();
    void send(const std::string &message);

private:
    void receive_loop();

    Endpoint peer_;
    sockaddr_in peer_addr_{};
    int fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread receive_thread_;
    std::function<void(std::string)> on_message_;
};
