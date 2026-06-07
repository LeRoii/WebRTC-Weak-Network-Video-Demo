#include "common/types.hpp"
#include "common/utils.hpp"
#include "webrtc/webrtc_session.hpp"

#include <csignal>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void print_usage(const char *program) {
    std::cerr
        << "Usage:\n"
        << "  " << program
        << " --role sender --local <ip:port> --peer <ip:port> --video-file <path>\n"
        << "  " << program
        << " --role receiver --local <ip:port> --peer <ip:port>"
           " [--output-file <path>]\n\n"
        << "Optional:\n"
        << "  --max-video-kbps <150-2000>       default: 2000\n"
        << "  --recovery-timeout-ms <100-1000>  default: 1000\n\n"
        << "Examples with scripts/netem_loss.sh ns-up:\n"
        << "  sudo ip netns exec webrtc_rx " << program
        << " --role receiver --local 10.88.0.2:9002 --peer 10.88.0.1:9001"
           " --output-file /tmp/received.h264\n"
        << "  sudo ip netns exec webrtc_tx " << program
        << " --role sender --local 10.88.0.1:9001 --peer 10.88.0.2:9002"
           " --video-file /tmp/input.mp4\n";
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        std::optional<Role> role;
        std::optional<Endpoint> local;
        std::optional<Endpoint> peer;
        std::string video_file;
        std::string output_file = "received.h264";
        int max_video_kbps = 2000;
        int recovery_timeout_ms = 1000;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--role" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "sender") {
                    role = Role::Sender;
                } else if (value == "receiver") {
                    role = Role::Receiver;
                } else {
                    throw std::runtime_error("--role must be sender or receiver");
                }
            } else if (arg == "--local" && i + 1 < argc) {
                local = parse_endpoint(argv[++i]);
            } else if (arg == "--peer" && i + 1 < argc) {
                peer = parse_endpoint(argv[++i]);
            } else if (arg == "--video-file" && i + 1 < argc) {
                video_file = argv[++i];
            } else if (arg == "--output-file" && i + 1 < argc) {
                output_file = argv[++i];
            } else if (arg == "--max-video-kbps" && i + 1 < argc) {
                max_video_kbps = std::stoi(argv[++i]);
                if (max_video_kbps < 150 || max_video_kbps > 2000) {
                    throw std::runtime_error(
                        "--max-video-kbps must be between 150 and 2000");
                }
            } else if (arg == "--recovery-timeout-ms" && i + 1 < argc) {
                recovery_timeout_ms = std::stoi(argv[++i]);
                if (recovery_timeout_ms < 100 ||
                    recovery_timeout_ms > 1000) {
                    throw std::runtime_error(
                        "--recovery-timeout-ms must be between 100 and 1000");
                }
            } else if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            } else {
                throw std::runtime_error("unknown or incomplete argument: " + arg);
            }
        }

        if (!role || !local || !peer) {
            print_usage(argv[0]);
            return 1;
        }
        if (*role == Role::Sender && video_file.empty()) {
            throw std::runtime_error("--video-file is required for sender");
        }

        WebRtcSession session(*role, *local, *peer, video_file, output_file,
                              max_video_kbps, recovery_timeout_ms);
        session.run();
    } catch (const std::exception &error) {
        std::cerr << "fatal: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
