#include "common/utils.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int) {
    g_stop_requested = 1;
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::vector<std::string> split(const std::string &text, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        parts.push_back(item);
    }
    return parts;
}

std::string hex_encode(const std::string &input) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (unsigned char c : input) {
        output.push_back(kHex[c >> 4]);
        output.push_back(kHex[c & 0x0f]);
    }
    return output;
}

std::string hex_decode(const std::string &input) {
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return 0;
    };

    std::string output;
    output.reserve(input.size() / 2);
    for (std::size_t i = 0; i + 1 < input.size(); i += 2) {
        output.push_back(static_cast<char>((value(input[i]) << 4) | value(input[i + 1])));
    }
    return output;
}

Endpoint parse_endpoint(const std::string &value) {
    const auto pos = value.find(':');
    if (pos == std::string::npos) {
        throw std::runtime_error("endpoint must be ip:port, got: " + value);
    }

    Endpoint endpoint;
    endpoint.ip = value.substr(0, pos);
    endpoint.port = static_cast<uint16_t>(std::stoul(value.substr(pos + 1)));
    return endpoint;
}
