#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

struct FrameTiming {
    uint16_t epoch = 0;
    uint32_t frame_id = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t sender_start_us = 0;
    int64_t first_packet_us = 0;
    int64_t frame_complete_us = 0;
    int64_t decode_complete_us = 0;
    int64_t present_us = 0;
    bool synchronized = false;
};

enum class FrameTerminalStatus {
    Presented,
    NetworkDropped,
    DecoderError,
    RendererDropped,
};

class FrameLatencyCsv {
public:
    void open(const std::string &path);
    void close();
    void record(const FrameTiming &timing, FrameTerminalStatus status);

private:
    static const char *status_name(FrameTerminalStatus status);
    static std::string elapsed_ms(uint32_t sender_start_us, int64_t event_us);
    static uint64_t frame_key(const FrameTiming &timing);

    std::mutex mutex_;
    std::ofstream output_;
    std::unordered_set<uint64_t> recorded_frames_;
};
