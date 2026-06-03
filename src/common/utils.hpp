#pragma once

#include "common/types.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

extern volatile std::sig_atomic_t g_stop_requested;

void handle_signal(int);
int64_t now_us();
std::vector<std::string> split(const std::string &text, char delimiter);
std::string hex_encode(const std::string &input);
std::string hex_decode(const std::string &input);
Endpoint parse_endpoint(const std::string &value);
