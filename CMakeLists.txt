cmake_minimum_required(VERSION 3.16)

project(webrtc_net_quality_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LibDataChannel CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_executable(webrtc_net_quality_probe
    src/common/utils.cpp
    src/main.cpp
    src/quality/media_statistics_handler.cpp
    src/quality/network_quality_estimator.cpp
    src/quality/video_adaptation.cpp
    src/signaling/udp_signaling.cpp
    src/video/video_file_reader.cpp
    src/video/h264_decoder.cpp
    src/video/video_receiver.cpp
    src/video/video_renderer.cpp
    src/video/video_sender.cpp
    src/video/video_stats.cpp
    src/webrtc/webrtc_session.cpp
)

target_include_directories(webrtc_net_quality_probe
    PRIVATE
        src
)

target_link_libraries(webrtc_net_quality_probe
    PRIVATE
        LibDataChannel::LibDataChannel
        PkgConfig::FFMPEG
        PkgConfig::SDL2
        Threads::Threads
)

target_compile_options(webrtc_net_quality_probe
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)
