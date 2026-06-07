# WebRTC Weak-Network Video Demo

Linux C++ sender/receiver demo for clean H264 video delivery over WebRTC under
packet loss.

The peers exchange SDP and ICE candidates over UDP signaling. Video and network
quality feedback use the WebRTC video track itself:

- RTP carries H264 media and per-packet frame metadata.
- RTCP RR and TWCC report media and transport loss.
- RTCP NACK requests missing packets, which are retransmitted as RTX.
- RTCP PLI requests a new keyframe when the receiver loses decoder sync.

The sender decodes the input with FFmpeg and re-encodes it in real time as
Constrained Baseline H264. The receiver buffers RTP packets by frame and only
passes complete frames to the H264 depacketizer and decoder. After an incomplete
frame times out, playback freezes on the last good image until a complete
keyframe restores synchronization. This favors a complete picture over
continuity and is intended for loss rates from 10% through severe 80% loss.

- RTT
- packet loss
- jitter
- send bitrate
- receive bitrate
- RTCP bitrate
- NACK, RTX, PLI, dropped-frame, and decoder-error counters
- adaptive video bitrate / frame rate / resolution
- received H264 bitrate / frame count on the receiver

The project uses [libdatachannel](https://github.com/paullouisageneau/libdatachannel),
a lightweight C++ WebRTC implementation.

## Structure

```text
src/common      shared types, argument helpers, time/encoding helpers
src/signaling   UDP signaling transport for SDP and ICE candidates
src/quality     RTP/RTCP/TWCC statistics, quality estimator, adaptation policy
src/video       FFmpeg transcode, RTP recovery, frame gate, decode and rendering
src/webrtc      PeerConnection, signaling, SDP and video Track orchestration
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
  --video-file input.mp4 \
  --max-video-kbps 2000
```

The sender prints network quality and the current adaptive video target. The
receiver opens an SDL2 window for real-time playback, prints received H264
statistics, and writes an Annex-B H264 elementary stream to `--output-file`.
The input may use any video codec supported by the installed FFmpeg build.

Optional controls:

```text
--max-video-kbps <150-2000>       maximum encoded media target, default 2000
--recovery-timeout-ms <100-1000>  incomplete-frame recovery window, default 1000
```

The adaptation ladder is:

```text
2000 kbps  1280x720  30 fps
1200 kbps   960x540  24 fps
 600 kbps   640x360  15 fps
 300 kbps   426x240  10 fps
 150 kbps   320x180   5 fps
```

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

After SDP/ICE completes and the sender starts printing quality logs, apply
one-way media loss:

```bash
sudo ./scripts/netem_loss.sh ns-loss 10
sudo ./scripts/netem_loss.sh ns-loss 40
sudo ./scripts/netem_loss.sh ns-loss 80
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

Validate that the received elementary stream contains no corrupt decoded
frames:

```bash
ffmpeg -v error -err_detect explode -i /tmp/received.h264 -f null -
```

The receiver requires access to the graphical desktop. When running it through
`sudo ip netns exec`, preserve the desktop environment variables required by
your X11 or Wayland session. For headless validation, set
`SDL_VIDEODRIVER=dummy`.
