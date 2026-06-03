#include "signaling/udp_signaling.hpp"

#include "common/utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

UdpSignaling::UdpSignaling(Endpoint local, Endpoint peer) : peer_(std::move(peer)) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("socket() failed");
    }

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(local.port);
    if (::inet_pton(AF_INET, local.ip.c_str(), &local_addr.sin_addr) != 1) {
        throw std::runtime_error("invalid local ip: " + local.ip);
    }

    if (::bind(fd_, reinterpret_cast<sockaddr *>(&local_addr), sizeof(local_addr)) < 0) {
        throw std::runtime_error("bind() failed: " + std::string(std::strerror(errno)));
    }

    peer_addr_.sin_family = AF_INET;
    peer_addr_.sin_port = htons(peer_.port);
    if (::inet_pton(AF_INET, peer_.ip.c_str(), &peer_addr_.sin_addr) != 1) {
        throw std::runtime_error("invalid peer ip: " + peer_.ip);
    }
}

UdpSignaling::~UdpSignaling() {
    stop();
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void UdpSignaling::start(std::function<void(std::string)> on_message) {
    on_message_ = std::move(on_message);
    receive_thread_ = std::thread([this] { receive_loop(); });
}

void UdpSignaling::stop() {
    stopping_.store(true);
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

void UdpSignaling::send(const std::string &message) {
    const auto sent = ::sendto(fd_, message.data(), message.size(), 0,
                               reinterpret_cast<sockaddr *>(&peer_addr_),
                               sizeof(peer_addr_));
    if (sent < 0) {
        std::cerr << "signaling_send_failed=" << std::strerror(errno) << std::endl;
    }
}

void UdpSignaling::receive_loop() {
    while (!stopping_.load() && !g_stop_requested) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200'000;

        const int ready = ::select(fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }

        char buffer[65535];
        const auto received = ::recvfrom(fd_, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received <= 0) {
            continue;
        }

        if (on_message_) {
            on_message_(std::string(buffer, buffer + received));
        }
    }
}
