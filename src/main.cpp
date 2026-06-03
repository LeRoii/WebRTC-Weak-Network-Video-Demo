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

        WebRtcSession session(*role, *local, *peer, video_file, output_file);
        session.run();
    } catch (const std::exception &error) {
        std::cerr << "fatal: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
