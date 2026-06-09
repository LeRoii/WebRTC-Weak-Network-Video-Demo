#include "video/frame_timing.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

std::string optional_integer(int64_t value) {
    return value > 0 ? std::to_string(value) : std::string();
}

}

void FrameLatencyCsv::open(const std::string &path) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_.open(path, std::ios::trunc);
    if (!output_) {
        throw std::runtime_error("cannot open latency CSV: " + path);
    }
    recorded_frames_.clear();
    output_
        << "epoch,frame_id,rtp_timestamp,sender_start_us32,"
           "first_packet_us,frame_complete_us,decode_complete_us,present_us,"
           "source_to_first_packet_ms,source_to_frame_complete_ms,"
           "source_to_decode_ms,source_to_present_ms,status,synchronized,"
           "network_dropped,renderer_dropped\n";
    output_.flush();
}

void FrameLatencyCsv::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    output_.close();
    recorded_frames_.clear();
}

void FrameLatencyCsv::record(const FrameTiming &timing,
                             FrameTerminalStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!output_) {
        return;
    }

    const uint64_t key = frame_key(timing);
    if (timing.frame_id != 0 && !recorded_frames_.insert(key).second) {
        return;
    }

    output_ << timing.epoch << ','
            << timing.frame_id << ','
            << timing.rtp_timestamp << ','
            << timing.sender_start_us << ','
            << optional_integer(timing.first_packet_us) << ','
            << optional_integer(timing.frame_complete_us) << ','
            << optional_integer(timing.decode_complete_us) << ','
            << optional_integer(timing.present_us) << ','
            << elapsed_ms(timing.sender_start_us, timing.first_packet_us) << ','
            << elapsed_ms(timing.sender_start_us, timing.frame_complete_us) << ','
            << elapsed_ms(timing.sender_start_us, timing.decode_complete_us) << ','
            << elapsed_ms(timing.sender_start_us, timing.present_us) << ','
            << status_name(status) << ','
            << (timing.synchronized ? 1 : 0) << ','
            << (status == FrameTerminalStatus::NetworkDropped ? 1 : 0) << ','
            << (status == FrameTerminalStatus::RendererDropped ? 1 : 0)
            << '\n';
    output_.flush();
}

const char *FrameLatencyCsv::status_name(FrameTerminalStatus status) {
    switch (status) {
    case FrameTerminalStatus::Presented:
        return "presented";
    case FrameTerminalStatus::NetworkDropped:
        return "network_dropped";
    case FrameTerminalStatus::DecoderError:
        return "decoder_error";
    case FrameTerminalStatus::RendererDropped:
        return "renderer_dropped";
    }
    return "unknown";
}

std::string FrameLatencyCsv::elapsed_ms(uint32_t sender_start_us,
                                        int64_t event_us) {
    if (sender_start_us == 0 || event_us <= 0) {
        return {};
    }
    const uint32_t event_low = static_cast<uint32_t>(event_us);
    const uint32_t elapsed_us = event_low - sender_start_us;
    std::ostringstream value;
    value << std::fixed << std::setprecision(3)
          << static_cast<double>(elapsed_us) / 1000.0;
    return value.str();
}

uint64_t FrameLatencyCsv::frame_key(const FrameTiming &timing) {
    return (static_cast<uint64_t>(timing.epoch) << 32U) | timing.frame_id;
}
