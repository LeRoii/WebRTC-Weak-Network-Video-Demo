#!/usr/bin/env python3

import argparse
import csv
import json
import os
import pwd
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NETEM = ROOT / "scripts" / "netem_loss.sh"
DEFAULT_INPUT = Path("/home/u20/code/jetson-2k.mp4")
DEFAULT_OUTPUT = ROOT / "demo" / "output" / "weak_network_demo_720p30.mp4"
FONT = "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc"

SENDER_RE = re.compile(
    r"transport_loss=(?P<loss>[0-9.]+)% .*"
    r"video_target=(?P<kbps>\d+)kbps/(?P<fps>\d+)fps/"
    r"(?P<width>\d+)x(?P<height>\d+)"
)
RECEIVER_RE = re.compile(r"sync=(?P<sync>[01])")
RECORDER_STARTED_RE = re.compile(
    r"demo_recording_started=.* start_us=(?P<start_us>\d+)"
)


class DemoRun:
    def __init__(self, args):
        self.args = args
        self.output = args.output.resolve()
        self.work = self.output.parent / "work"
        self.raw_recording = self.work / "receiver_720p30.mp4"
        self.source_recording = self.work / "source_720p30.mp4"
        self.received_h264 = self.work / "received.h264"
        self.ass_file = self.work / "dashboard.ass"
        self.timeline_file = self.work / "timeline.json"
        self.latency_csv = self.output.with_name(
            self.output.stem + "_latency.csv"
        )
        self.latency_summary_file = self.output.with_name(
            self.output.stem + "_latency_summary.json"
        )
        self.log_file = self.work / "run.log"
        self.processes = []
        self.reader_threads = []
        self.log_lock = threading.Lock()
        self.events = []
        self.demo_started = threading.Event()
        self.demo_start_time = None
        self.recording_start_us = None
        self.source_recording_start_us = None
        self.namespaces_created = False

    def run(self):
        self.prepare()
        try:
            self.setup_namespaces()
            self.start_peers()
            self.wait_for_recording()
            self.run_stages()
        finally:
            self.stop_peers()
            self.cleanup_namespaces()
            self.finish_readers()

        self.validate_raw_recording()
        self.validate_latency_csv()
        self.write_timeline()
        self.write_latency_summary()
        self.write_ass()
        self.compose()
        self.validate_final_video()
        print(f"Demo video: {self.output}")

    def prepare(self):
        if not self.args.input.is_file():
            raise RuntimeError(f"input video not found: {self.args.input}")
        for tool in ("cmake", "ffmpeg", "ffprobe", "sudo", "ip", "tc"):
            if not shutil.which(tool):
                raise RuntimeError(f"required tool not found: {tool}")
        run_checked(["sudo", "-n", "true"])

        self.output.parent.mkdir(parents=True, exist_ok=True)
        if self.work.exists():
            shutil.rmtree(self.work)
        self.work.mkdir(parents=True)

        if not self.args.skip_build:
            run_checked(["cmake", "-S", str(ROOT), "-B", str(ROOT / "build")])
            run_checked(
                ["cmake", "--build", str(ROOT / "build"), "-j", str(self.args.jobs)]
            )

    def setup_namespaces(self):
        existing = existing_demo_namespaces()
        if existing:
            active = {
                namespace: namespace_pids(namespace)
                for namespace in existing
                if namespace_pids(namespace)
            }
            if active:
                details = ", ".join(
                    f"{namespace} ({' '.join(pids)})"
                    for namespace, pids in active.items()
                )
                raise RuntimeError(
                    "demo namespaces contain active processes: " + details
                )
            print("Cleaning stale demo namespaces:", ", ".join(existing))
            run_checked(["sudo", "-n", str(NETEM), "ns-down"], cwd=ROOT)
        run_checked(["sudo", "-n", str(NETEM), "ns-up"], cwd=ROOT)
        self.namespaces_created = True

    def start_peers(self):
        binary = ROOT / "build" / "webrtc_net_quality_probe"
        receiver = namespace_user_prefix("webrtc_rx")
        if self.args.headless:
            receiver += ["env", "SDL_VIDEODRIVER=dummy"]
        else:
            desktop = discover_desktop_environment()
            if desktop:
                print(
                    "Using desktop session:",
                    " ".join(f"{key}={value}" for key, value in desktop.items()),
                )
                receiver += ["env", *[f"{key}={value}" for key, value in desktop.items()]]
            else:
                print("Desktop session not detected; using SDL dummy driver")
                receiver += ["env", "SDL_VIDEODRIVER=dummy"]
        receiver += [
            str(binary),
            "--role",
            "receiver",
            "--local",
            "10.88.0.2:9002",
            "--peer",
            "10.88.0.1:9001",
            "--output-file",
            str(self.received_h264),
            "--demo-record-file",
            str(self.raw_recording),
            "--latency-csv",
            str(self.latency_csv),
        ]
        sender = namespace_user_prefix("webrtc_tx") + [
            str(binary),
            "--role",
            "sender",
            "--local",
            "10.88.0.1:9001",
            "--peer",
            "10.88.0.2:9002",
            "--video-file",
            str(self.args.input.resolve()),
        ]
        if not self.args.no_source_preview:
            sender += [
                "--demo-source-record-file",
                str(self.source_recording),
            ]
        self.processes.append(("receiver", self.start_process(receiver)))
        time.sleep(0.5)
        self.processes.append(("sender", self.start_process(sender)))

    def start_process(self, command):
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        name = "receiver" if "webrtc_rx" in command else "sender"
        thread = threading.Thread(
            target=self.read_process_output, args=(name, process), daemon=True
        )
        thread.start()
        self.reader_threads.append(thread)
        return process

    def read_process_output(self, name, process):
        assert process.stdout is not None
        with self.log_file.open("a", encoding="utf-8") as log:
            for raw_line in process.stdout:
                line = raw_line.rstrip()
                now = time.monotonic()
                recorder_started = RECORDER_STARTED_RE.search(line)
                if recorder_started:
                    with self.log_lock:
                        start_us = int(recorder_started.group("start_us"))
                        if name == "sender":
                            self.source_recording_start_us = start_us
                        elif self.demo_start_time is None:
                            self.demo_start_time = now
                            self.recording_start_us = start_us
                            self.demo_started.set()
                with self.log_lock:
                    at = (
                        now - self.demo_start_time
                        if self.demo_start_time is not None
                        else None
                    )
                    record = {"type": "log", "source": name, "at": at, "line": line}
                    self.events.append(record)
                    prefix = f"{at:8.3f}s" if at is not None else "  pre-roll"
                    log.write(f"[{prefix}] [{name}] {line}\n")
                    log.flush()

    def wait_for_recording(self):
        deadline = time.monotonic() + self.args.start_timeout
        while time.monotonic() < deadline:
            if self.demo_started.wait(timeout=0.2):
                self.add_stage("NORMAL NETWORK", 0)
                return
            exited = [
                f"{name}={process.returncode}"
                for name, process in self.processes
                if process.poll() is not None
            ]
            if exited:
                raise RuntimeError(
                    "peer exited before recording started: " + ", ".join(exited)
                )
        raise RuntimeError("timed out waiting for the first recorded video frame")

    def run_stages(self):
        normal, loss_30, loss_50, recovery = self.args.durations
        print(f"NORMAL NETWORK: {normal}s")
        sleep_until(self.demo_start_time + normal)

        run_checked(
            ["sudo", "-n", str(NETEM), "ns-loss", "30", "0", "tx"], cwd=ROOT
        )
        self.add_stage("30% PACKET LOSS", 30)
        print(f"30% PACKET LOSS: {loss_30}s")
        sleep_until(self.stage_start_time() + loss_30)

        run_checked(
            ["sudo", "-n", str(NETEM), "ns-loss", "50", "0", "tx"], cwd=ROOT
        )
        self.add_stage("50% PACKET LOSS", 50)
        print(f"50% PACKET LOSS: {loss_50}s")
        sleep_until(self.stage_start_time() + loss_50)

        run_checked(["sudo", "-n", str(NETEM), "ns-clear", "tx"], cwd=ROOT)
        self.add_stage("NETWORK RECOVERY", 0)
        print(f"NETWORK RECOVERY: {recovery}s")
        sleep_until(self.stage_start_time() + recovery)

    def add_stage(self, label, loss):
        with self.log_lock:
            at = time.monotonic() - self.demo_start_time
            self.events.append(
                {"type": "stage", "at": at, "label": label, "loss": loss}
            )

    def stage_start_time(self):
        with self.log_lock:
            stage = next(
                event for event in reversed(self.events) if event["type"] == "stage"
            )
        return self.demo_start_time + stage["at"]

    def stop_peers(self):
        for namespace in ("webrtc_tx", "webrtc_rx"):
            result = subprocess.run(
                ["sudo", "-n", "ip", "netns", "pids", namespace],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            pids = result.stdout.split()
            if pids:
                subprocess.run(
                    ["sudo", "-n", "kill", "-INT", *pids],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )

        for _, process in self.processes:
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()

    def cleanup_namespaces(self):
        if self.namespaces_created:
            subprocess.run(
                ["sudo", "-n", str(NETEM), "ns-down"],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            self.namespaces_created = False

    def finish_readers(self):
        for thread in self.reader_threads:
            thread.join(timeout=2)

    def validate_raw_recording(self):
        if not self.raw_recording.is_file() or self.raw_recording.stat().st_size == 0:
            raise RuntimeError("receiver demo recording was not created")
        probe = probe_video(self.raw_recording)
        assert_video_shape(probe, self.raw_recording)
        assert_constant_frame_rate(self.raw_recording)
        if not self.args.no_source_preview:
            if (not self.source_recording.is_file() or
                    self.source_recording.stat().st_size == 0):
                raise RuntimeError("sender source recording was not created")
            source_probe = probe_video(self.source_recording)
            assert_video_shape(source_probe, self.source_recording)
            assert_constant_frame_rate(self.source_recording)
            if self.source_recording_start_us is None:
                raise RuntimeError(
                    "sender source recording start timestamp is unavailable"
                )

    def validate_latency_csv(self):
        samples = self.load_latency_samples()
        if not samples:
            raise RuntimeError("latency CSV contains no presented frames")
        frame_keys = [(sample["epoch"], sample["frame_id"]) for sample in samples]
        if len(frame_keys) != len(set(frame_keys)):
            raise RuntimeError("latency CSV contains duplicate presented frames")
        if any(sample["latency_ms"] < 0 for sample in samples):
            raise RuntimeError("latency CSV contains negative latency")
        if self.recording_start_us is None:
            raise RuntimeError("demo recording start timestamp is unavailable")

    def write_timeline(self):
        with self.log_lock:
            events = list(self.events)
        self.timeline_file.write_text(
            json.dumps(events, indent=2, ensure_ascii=True) + "\n",
            encoding="utf-8",
        )

    def load_latency_samples(self):
        if not self.latency_csv.is_file():
            return []
        samples = []
        with self.latency_csv.open(newline="", encoding="utf-8") as source:
            for row in csv.DictReader(source):
                if row["status"] != "presented" or not row["source_to_present_ms"]:
                    continue
                present_us = int(row["present_us"])
                video_at = (
                    (present_us - self.recording_start_us) / 1_000_000.0
                    if self.recording_start_us is not None
                    else 0.0
                )
                samples.append(
                    {
                        "at": max(0.0, video_at),
                        "epoch": int(row["epoch"]),
                        "frame_id": int(row["frame_id"]),
                        "latency_ms": float(row["source_to_present_ms"]),
                    }
                )
        samples.sort(key=lambda sample: (sample["at"], sample["frame_id"]))
        return samples

    def target_duration(self):
        return min(
            probe_duration(self.raw_recording),
            sum(self.args.durations),
        )

    def write_latency_summary(self):
        duration = self.target_duration()
        samples = [
            sample
            for sample in self.load_latency_samples()
            if sample["at"] <= duration
        ]
        values = sorted(sample["latency_ms"] for sample in samples)
        summary = {
            "presented_frames": len(values),
            "p50_ms": percentile(values, 50),
            "p95_ms": percentile(values, 95),
            "max_ms": max(values),
        }
        self.latency_summary_file.write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8"
        )
        print(
            "E2E display latency: "
            f"P50={summary['p50_ms']:.3f} ms "
            f"P95={summary['p95_ms']:.3f} ms "
            f"max={summary['max_ms']:.3f} ms "
            f"frames={summary['presented_frames']}"
        )

    def write_ass(self):
        duration = self.target_duration()
        latency_samples = [
            sample
            for sample in self.load_latency_samples()
            if sample["at"] <= duration
        ]
        sender_samples = []
        receiver_samples = []
        stages = []
        with self.log_lock:
            events = list(self.events)
        for event in events:
            if event["type"] == "stage":
                stages.append(event)
                continue
            if event["at"] is None or event["at"] < 0:
                continue
            sender = SENDER_RE.search(event["line"])
            if event["source"] == "sender" and sender:
                sender_samples.append(
                    {
                        "at": event["at"],
                        "loss": float(sender.group("loss")),
                        "kbps": int(sender.group("kbps")),
                        "fps": int(sender.group("fps")),
                        "width": int(sender.group("width")),
                        "height": int(sender.group("height")),
                    }
                )
            receiver = RECEIVER_RE.search(event["line"])
            if event["source"] == "receiver" and receiver:
                receiver_samples.append(
                    {"at": event["at"], "sync": receiver.group("sync") == "1"}
                )
        sender_samples.sort(key=lambda sample: sample["at"])
        receiver_samples.sort(key=lambda sample: sample["at"])
        stages.sort(key=lambda stage: stage["at"])

        source_style = (
            "Style: Source,Noto Sans,20,&H00FFFFFF,&H00FFFFFF,&H00101010,"
            "&H70000000,1,0,0,0,100,100,0,0,3,1,0,9,24,24,214,1\n"
            if not self.args.no_source_preview
            else ""
        )
        header = f"""[Script Info]
ScriptType: v4.00+
PlayResX: 1280
PlayResY: 720
ScaledBorderAndShadow: yes

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Dashboard,Noto Sans,25,&H00FFFFFF,&H00FFFFFF,&H00202020,&H78000000,0,0,0,0,100,100,0,0,3,1,0,7,24,24,24,1
Style: Banner,Noto Sans,42,&H00FFFFFF,&H00FFFFFF,&H00101010,&H50000000,1,0,0,0,100,100,1,0,3,2,0,8,20,20,300,1
{source_style}
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""
        lines = [header]
        for index, latency in enumerate(latency_samples):
            start = min(latency["at"], duration)
            end = (
                min(latency_samples[index + 1]["at"], duration)
                if index + 1 < len(latency_samples)
                else duration
            )
            if end <= start:
                continue
            at = min(start + 0.001, duration)
            stage = latest_at(stages, at) or {
                "label": "NORMAL NETWORK",
                "loss": 0,
                "at": 0,
            }
            sender = latest_at(sender_samples, at)
            receiver = latest_at(receiver_samples, at)
            measured_loss = rolling_loss_average(
                sender_samples, at, max(stage["at"], at - 5.0)
            )
            measured = (
                f"{measured_loss:.1f}%"
                if measured_loss is not None
                else "collecting..."
            )
            profile = (
                f"{sender['kbps']} kbps / {sender['width']}x{sender['height']} / "
                f"{sender['fps']} fps"
                if sender
                else "collecting..."
            )
            sync = (
                "SYNCHRONIZED"
                if receiver and receiver["sync"]
                else "RECOVERING"
            )
            text = (
                f"Injected Packet Loss: {stage['loss']}%\\N"
                f"Measured Transport Loss (5s avg): {measured}\\N"
                f"Video Profile: {profile}\\N"
                f"Sync Status: {sync}\\N"
                f"Frame ID: {latency['epoch']}:{latency['frame_id']}\\N"
                f"E2E Display Latency: {latency['latency_ms']:.1f} ms"
            )
            lines.append(
                ass_dialogue(start, end, "Dashboard", text)
            )

        for index, stage in enumerate(stages):
            end = min(stage["at"] + 2.5, duration)
            lines.append(
                ass_dialogue(stage["at"], end, "Banner", stage["label"])
            )
        if not self.args.no_source_preview:
            lines.append(
                ass_dialogue(
                    0, duration, "Source", "SOURCE VIDEO  |  720p / 30 fps"
                )
            )
        self.ass_file.write_text("".join(lines), encoding="utf-8")

    def compose(self):
        duration = self.target_duration()
        escaped_ass = ffmpeg_filter_escape(self.ass_file)
        command = [
            "ffmpeg",
            "-y",
            "-v",
            "warning",
            "-i",
            str(self.raw_recording),
        ]
        if self.args.no_source_preview:
            command += [
                "-vf",
                f"subtitles='{escaped_ass}'",
            ]
        else:
            font_file = ffmpeg_filter_escape(Path(FONT))
            source_trim = max(
                0.0,
                (self.recording_start_us - self.source_recording_start_us)
                / 1_000_000.0,
            )
            filter_graph = (
                "[0:v]setpts=PTS-STARTPTS[received];"
                f"[1:v]trim=start={source_trim:.6f}:"
                f"duration={duration:.3f},setpts=PTS-STARTPTS,"
                "scale=320:180:force_original_aspect_ratio=decrease,"
                "pad=320:180:(ow-iw)/2:(oh-ih)/2:black,"
                f"drawtext=fontfile='{font_file}':"
                "text='SOURCE  %{pts\\:hms}':fontcolor=white:fontsize=18:"
                "box=1:boxcolor=black@0.65:boxborderw=5:x=8:y=h-th-8[source];"
                "[received][source]overlay=x=W-w-24:y=24:"
                "eof_action=pass:shortest=0[layout];"
                "[layout]drawbox=x=iw-344:y=20:w=328:h=188:"
                "color=white@0.85:t=2,"
                f"subtitles='{escaped_ass}'[out]"
            )
            command += [
                "-i",
                str(self.source_recording),
                "-filter_complex",
                filter_graph,
                "-map",
                "[out]",
            ]
        command += [
                "-an",
                "-r",
                "30",
                "-c:v",
                "libx264",
                "-preset",
                "medium",
                "-crf",
                "18",
                "-pix_fmt",
                "yuv420p",
                "-movflags",
                "+faststart",
                "-t",
                f"{duration:.3f}",
                str(self.output),
            ]
        run_checked(command)

    def validate_final_video(self):
        probe = probe_video(self.output)
        assert_video_shape(probe, self.output)
        assert_constant_frame_rate(self.output)
        run_checked(
            [
                "ffmpeg",
                "-v",
                "error",
                "-err_detect",
                "explode",
                "-i",
                str(self.output),
                "-f",
                "null",
                "-",
            ]
        )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Generate the WebRTC weak-network demonstration video."
    )
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--durations",
        type=parse_durations,
        default=(10.0, 15.0, 20.0, 20.0),
        metavar="NORMAL,LOSS30,LOSS50,RECOVERY",
        help="stage durations in seconds (default: 10,15,20,20)",
    )
    parser.add_argument("--start-timeout", type=float, default=30.0)
    parser.add_argument("--jobs", type=int, default=max(1, min(os.cpu_count() or 2, 4)))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--headless",
        action="store_true",
        help="disable the live SDL receiver window",
    )
    parser.add_argument(
        "--no-source-preview",
        action="store_true",
        help="omit the source-video preview from the composed demo",
    )
    return parser.parse_args()


def parse_durations(value):
    try:
        durations = tuple(float(item) for item in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("durations must be numeric") from error
    if len(durations) != 4 or any(item <= 0 for item in durations):
        raise argparse.ArgumentTypeError(
            "durations must contain four positive values"
        )
    return durations


def run_checked(command, cwd=None):
    print("+", " ".join(str(part) for part in command))
    subprocess.run(command, cwd=cwd, check=True)


def existing_demo_namespaces():
    result = subprocess.run(
        ["ip", "netns", "list"],
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    )
    names = {line.split()[0] for line in result.stdout.splitlines() if line.split()}
    return [
        namespace
        for namespace in ("webrtc_tx", "webrtc_rx")
        if namespace in names
    ]


def namespace_pids(namespace):
    result = subprocess.run(
        ["sudo", "-n", "ip", "netns", "pids", namespace],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.stdout.split()


def namespace_user_prefix(namespace):
    user = pwd.getpwuid(os.getuid()).pw_name
    return [
        "sudo",
        "-n",
        "ip",
        "netns",
        "exec",
        namespace,
        "sudo",
        "-n",
        "-u",
        user,
        "--",
    ]


def discover_desktop_environment():
    keys = (
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "XDG_RUNTIME_DIR",
        "XAUTHORITY",
        "DBUS_SESSION_BUS_ADDRESS",
    )
    environment = {key: os.environ[key] for key in keys if os.environ.get(key)}
    if environment.get("DISPLAY") or environment.get("WAYLAND_DISPLAY"):
        return environment

    uid = os.getuid()
    candidates = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != uid:
                continue
            values = {}
            for item in (entry / "environ").read_bytes().split(b"\0"):
                if b"=" not in item:
                    continue
                key, value = item.split(b"=", 1)
                name = key.decode(errors="ignore")
                if name in keys:
                    values[name] = value.decode(errors="ignore")
            if values.get("DISPLAY") or values.get("WAYLAND_DISPLAY"):
                candidates.append(values)
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    if not candidates:
        return {}
    return max(candidates, key=lambda values: len(values))


def sleep_until(deadline):
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.2))


def latest_at(samples, at):
    result = None
    for sample in samples:
        if sample["at"] <= at:
            result = sample
        else:
            break
    return result


def rolling_loss_average(samples, at, start):
    values = [
        sample["loss"]
        for sample in samples
        if start <= sample["at"] <= at
    ]
    return sum(values) / len(values) if values else None


def percentile(values, percent):
    if not values:
        raise RuntimeError("cannot calculate latency percentile without samples")
    if len(values) == 1:
        return values[0]
    position = (len(values) - 1) * percent / 100.0
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    fraction = position - lower
    return values[lower] * (1.0 - fraction) + values[upper] * fraction


def ass_time(seconds):
    total_centiseconds = max(0, int(round(seconds * 100)))
    hours, remainder = divmod(total_centiseconds, 360000)
    minutes, remainder = divmod(remainder, 6000)
    whole_seconds, centiseconds = divmod(remainder, 100)
    return f"{hours}:{minutes:02d}:{whole_seconds:02d}.{centiseconds:02d}"


def ass_dialogue(start, end, style, text):
    escaped = text.replace("{", r"\{").replace("}", r"\}")
    return (
        f"Dialogue: 0,{ass_time(start)},{ass_time(end)},{style},,"
        f"0,0,0,,{escaped}\n"
    )


def ffmpeg_filter_escape(path):
    return str(path.resolve()).replace("\\", "\\\\").replace(":", r"\:").replace("'", r"\'")


def probe_video(path):
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height,pix_fmt,r_frame_rate,avg_frame_rate,"
            "nb_frames,duration",
            "-of",
            "json",
            str(path),
        ],
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    )
    streams = json.loads(result.stdout).get("streams", [])
    if not streams:
        raise RuntimeError(f"no video stream found: {path}")
    return streams[0]


def probe_duration(path):
    probe = probe_video(path)
    duration = probe.get("duration")
    if duration is None:
        raise RuntimeError(f"video duration unavailable: {path}")
    return float(duration)


def assert_video_shape(probe, path):
    if probe.get("codec_name") != "h264":
        raise RuntimeError(f"{path} is not H264: {probe}")
    if probe.get("width") != 1280 or probe.get("height") != 720:
        raise RuntimeError(f"{path} is not 1280x720: {probe}")
    if probe.get("pix_fmt") != "yuv420p":
        raise RuntimeError(f"{path} is not yuv420p: {probe}")
    if probe.get("r_frame_rate") != "30/1":
        raise RuntimeError(f"{path} is not 30fps: {probe}")


def assert_constant_frame_rate(path):
    result = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "frame=pkt_duration_time",
            "-of",
            "csv=p=0",
            str(path),
        ],
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    )
    durations = [
        float(line)
        for line in result.stdout.splitlines()
        if line.strip() and line.strip() != "N/A"
    ]
    if not durations:
        raise RuntimeError(f"frame durations unavailable: {path}")
    expected = 1.0 / 30.0
    if any(abs(duration - expected) > 0.00001 for duration in durations):
        raise RuntimeError(f"{path} does not have constant 30fps frame timing")


def main():
    args = parse_args()
    try:
        DemoRun(args).run()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
