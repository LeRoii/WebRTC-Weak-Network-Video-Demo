# WebRTC Network Quality Probe

Linux C++ demo project for WebRTC send/receive network quality probing.

This version runs as either a sender process or a receiver process. The two
processes exchange WebRTC signaling over UDP, open a DataChannel, send probe
packets at a fixed interval, echo ACKs back, and print real-time network quality
metrics on the sender. It also negotiates a WebRTC video track and sends an H264
video stream read from a local file from sender to receiver.

- RTT
- packet loss
- jitter
- send bitrate
- receive bitrate
- acknowledged bitrate
- suggested video bitrate / frame rate / resolution
- received H264 bitrate / frame count on the receiver

The project uses [libdatachannel](https://github.com/paullouisageneau/libdatachannel),
a lightweight C++ WebRTC implementation.

## Structure

```text
src/common      shared types, argument helpers, time/encoding helpers
src/signaling   UDP signaling transport for SDP and ICE candidates
src/quality     probe packets, network quality estimator, adaptation policy
src/video       FFmpeg file reader, H264 RTP sender/depacketizer, receiver output
src/webrtc      PeerConnection, DataChannel, video Track orchestration
src/main.cpp    command-line parsing and process startup
```

## Dependencies

Install libdatachannel, FFmpeg development packages, and SDL2 development
packages. CMake must be able to find `LibDataChannelConfig.cmake`,
`libavcodec`, `libavformat`, `libavutil`, `libswscale`, and `sdl2`.

For Debian or Ubuntu, FFmpeg headers are typically installed with:

```bash
sudo apt install pkg-config libavcodec-dev libavformat-dev libavutil-dev \
  libswscale-dev libsdl2-dev
```

Example build flow after dependencies are installed:

```bash
cmake -S . -B build
cmake --build build
```

Run two processes on the same host without namespaces:

```bash
./build/webrtc_net_quality_probe \
  --role receiver \
  --local 127.0.0.1:9002 \
  --peer 127.0.0.1:9001 \
  --output-file received.h264

./build/webrtc_net_quality_probe \
  --role sender \
  --local 127.0.0.1:9001 \
  --peer 127.0.0.1:9002 \
  --video-file input.mp4
```

The sender prints network quality and the current adaptive video target. The
receiver opens an SDL2 window for real-time playback, prints received H264
statistics, and writes an Annex-B H264 elementary stream to `--output-file`.
The input file must contain an H264 video stream. Common MP4, MKV, MOV, and raw
H264 files are supported through FFmpeg.

Play the received file with:

```bash
ffplay received.h264
```

## Notes

To test adaptation behavior on Linux, run the peers in network namespaces over a
veth pair and apply `tc netem`.

This repository includes a helper script:

```bash
sudo ./scripts/netem_loss.sh ns-up
```

Start the receiver first:

```bash
sudo ip netns exec webrtc_rx ./build/webrtc_net_quality_probe \
  --role receiver \
  --local 10.88.0.2:9002 \
  --peer 10.88.0.1:9001 \
  --output-file /tmp/received.h264
```

Start the sender in another terminal:

```bash
sudo ip netns exec webrtc_tx ./build/webrtc_net_quality_probe \
  --role sender \
  --local 10.88.0.1:9001 \
  --peer 10.88.0.2:9002 \
  --video-file /tmp/input.mp4
```

After the sender starts printing quality logs, apply one-way media loss:

```bash
sudo ./scripts/netem_loss.sh ns-loss 15
sudo ./scripts/netem_loss.sh ns-show
```

Clear or delete the test network:

```bash
sudo ./scripts/netem_loss.sh ns-clear
sudo ./scripts/netem_loss.sh ns-down
```

The namespace topology is:

```text
webrtc_tx/veth_tx 10.88.0.1/24  <---->  webrtc_rx/veth_rx 10.88.0.2/24
```

`ns-loss` defaults to applying loss only on `webrtc_tx/veth_tx`, so it simulates
one-way media loss from sender to receiver.

The receiver requires access to the graphical desktop. When running it through
`sudo ip netns exec`, preserve the desktop environment variables required by
your X11 or Wayland session.
