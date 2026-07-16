#!/usr/bin/env python3
import argparse
import os
import random
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def default_paths() -> tuple[Path, Path]:
    repo_root = Path(__file__).resolve().parents[1]
    return (
        repo_root / "tools" / "nsfw_scan_ahead.py",
        repo_root / "vlc-portable" / "vlc.exe",
    )


def default_state_dir(input_path: Path) -> Path:
    root = Path(
        os.environ.get("LOCALAPPDATA")
        or os.environ.get("TEMP")
        or tempfile.gettempdir()
    )
    return root / "icop" / "scan_guard" / input_path.stem


def read_status(path: Path) -> dict[str, str]:
    data: dict[str, str] = {}
    if not path.exists():
        return data
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            data[key.strip()] = value.strip()
    return data


def wait_for_initial_buffer(status_path: Path, buffer_ms: int, poll_seconds: float) -> None:
    while True:
        status = read_status(status_path)
        scanned_ms = int(status.get("last_scanned_ms", "0") or "0")
        done = status.get("done", "0") == "1"
        print(f"scanner progress: {scanned_ms / 1000:.1f}s buffered", flush=True)
        if scanned_ms >= buffer_ms or done:
            return
        time.sleep(poll_seconds)


class RcClient:
    def __init__(self, host: str, port: int) -> None:
        self._socket = socket.create_connection((host, port), timeout=10)
        self._socket.settimeout(2)
        self._last_state: str | None = None
        self._drain()

    def _drain(self) -> str:
        chunks: list[bytes] = []
        while True:
            try:
                chunk = self._socket.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            chunks.append(chunk)
            if len(chunk) < 4096:
                break
        return b"".join(chunks).decode("utf-8", errors="replace")

    def command(self, text: str) -> str:
        self._socket.sendall((text + "\n").encode("utf-8"))
        time.sleep(0.1)
        return self._drain()

    def _parse_state(self, response: str) -> str | None:
        state: str | None = None
        for raw_line in response.replace("\r", "\n").splitlines():
            line = raw_line.strip().lower()
            if "pause state:" in line:
                state = "paused"
            elif "play state:" in line:
                state = "playing"
            elif "stop state:" in line:
                state = "stopped"
        if state is not None:
            self._last_state = state
        return state

    def get_time_seconds(self) -> int | None:
        response = self.command("get_time")
        for token in response.replace("\r", "\n").split():
            if token.isdigit():
                return int(token)
        return None

    def get_state(self) -> str | None:
        state = self._parse_state(self.command("status"))
        if state is not None:
            return state
        return self._last_state

    def toggle_pause(self) -> None:
        response = self.command("pause")
        self._parse_state(response)

    def set_paused(self, desired_paused: bool) -> bool:
        state = self.get_state()
        if desired_paused:
            if state == "paused":
                return False
            self.toggle_pause()
            return True

        if state == "playing":
            return False
        self.toggle_pause()
        return True

    def close(self) -> None:
        try:
            self._socket.close()
        except OSError:
            pass


def launch_scanner(args: argparse.Namespace, map_path: Path, status_path: Path) -> subprocess.Popen[str]:
    command = [
        sys.executable,
        str(Path(args.scan_script)),
        "--input",
        str(Path(args.input)),
        "--output",
        str(map_path),
        "--status",
        str(status_path),
        "--sample-fps",
        str(args.sample_fps),
        "--hold-seconds",
        str(args.hold_seconds),
        "--threshold",
        str(args.threshold),
        "--provider",
        args.provider,
        "--write-every",
        str(args.write_every),
    ]
    if args.core_dll:
        command.extend(["--core-dll", args.core_dll])
    if args.model_path:
        command.extend(["--model-path", args.model_path])
    return subprocess.Popen(command)


def launch_vlc(args: argparse.Namespace, rc_port: int, map_path: Path, status_path: Path) -> subprocess.Popen[str]:
    env = os.environ.copy()
    env["NSFW_DECISION_MAP_PATH"] = str(map_path)
    env["NSFW_SCAN_STATUS_PATH"] = str(status_path)
    env["NSFW_DECISION_RELOAD_FRAMES"] = str(args.reload_frames)
    command = [
        str(Path(args.vlc)),
        "--extraintf",
        "rc",
        "--rc-host",
        f"127.0.0.1:{rc_port}",
        "--rc-quiet",
        "--video-filter=icop",
    ]
    if args.vlc_arg:
        command.extend(args.vlc_arg)
    command.append(str(Path(args.input)))
    return subprocess.Popen(command, env=env)


def connect_rc(port: int, retries: int, delay_seconds: float) -> RcClient:
    last_error: Exception | None = None
    for _ in range(retries):
        try:
            return RcClient("127.0.0.1", port)
        except OSError as exc:
            last_error = exc
            time.sleep(delay_seconds)
    raise RuntimeError(f"failed to connect to VLC RC interface: {last_error}")


def guard_playback(args: argparse.Namespace, scanner: subprocess.Popen[str], vlc_process: subprocess.Popen[str], rc: RcClient, status_path: Path, target_ms: int) -> int:
    poll_seconds = args.poll_seconds
    last_guard_mode: bool | None = None
    try:
        while vlc_process.poll() is None:
            status = read_status(status_path)
            scanned_ms = int(status.get("last_scanned_ms", "0") or "0")
            done = status.get("done", "0") == "1"
            playback_seconds = rc.get_time_seconds()
            if playback_seconds is None:
                time.sleep(poll_seconds)
                continue
            playback_ms = playback_seconds * 1000
            ahead_ms = scanned_ms - playback_ms

            should_pause = not done and ahead_ms < target_ms
            changed = rc.set_paused(should_pause)

            if should_pause and changed:
                print(f"buffer low: {ahead_ms / 1000:.1f}s ahead, pausing", flush=True)
            elif changed:
                print(f"buffer restored: {ahead_ms / 1000:.1f}s ahead, resuming", flush=True)
            elif last_guard_mode is None or should_pause != last_guard_mode:
                if should_pause:
                    print(f"buffer low: {ahead_ms / 1000:.1f}s ahead, already paused", flush=True)
                else:
                    print(f"buffer healthy: {ahead_ms / 1000:.1f}s ahead", flush=True)

            last_guard_mode = should_pause

            if scanner.poll() not in (None, 0):
                raise RuntimeError("scanner exited with failure")
            time.sleep(poll_seconds)

        return vlc_process.returncode or 0
    finally:
        rc.close()
        if scanner.poll() is None:
            scanner.terminate()
            try:
                scanner.wait(timeout=5)
            except subprocess.TimeoutExpired:
                scanner.kill()
                scanner.wait()


def build_parser() -> argparse.ArgumentParser:
    scan_script, vlc_path = default_paths()
    parser = argparse.ArgumentParser(description="Launch VLC with ahead-of-time NSFW scanning and buffer guarding.")
    parser.add_argument("--input", required=True, help="Video file to play.")
    parser.add_argument("--vlc", default=str(vlc_path), help="Path to vlc.exe.")
    parser.add_argument("--scan-script", default=str(scan_script), help="Path to nsfw_scan_ahead.py.")
    parser.add_argument("--core-dll", help="Path to icop_core.dll.")
    parser.add_argument("--model-path", help="Optional ONNX model path.")
    parser.add_argument("--provider", default="gpu", help="ONNX provider preference.")
    parser.add_argument("--threshold", type=float, default=0.5, help="Detection threshold.")
    parser.add_argument("--sample-fps", type=float, default=3.0, help="Ahead scan sampling FPS.")
    parser.add_argument("--hold-seconds", type=float, default=0.4, help="Blocked hold after a positive sample.")
    parser.add_argument("--buffer-seconds", type=float, default=10.0, help="Required scanned-ahead watermark before playback runs.")
    parser.add_argument("--write-every", type=int, default=3, help="Scanner write cadence in samples.")
    parser.add_argument("--reload-frames", type=int, default=12, help="Plugin decision-map reload cadence in rendered frames.")
    parser.add_argument("--poll-seconds", type=float, default=0.75, help="Controller polling interval.")
    parser.add_argument("--state-dir", help="Directory for temporary decision map and status files.")
    parser.add_argument("--vlc-arg", action="append", default=[], help="Additional VLC argument. Repeat for multiple values.")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    input_path = Path(args.input).resolve()
    state_dir = Path(args.state_dir) if args.state_dir else default_state_dir(input_path)
    state_dir.mkdir(parents=True, exist_ok=True)
    stem = input_path.stem
    map_path = state_dir / f"{stem}.nsfwmap"
    status_path = state_dir / f"{stem}.scanstatus"
    rc_port = random.randint(4212, 4999)
    target_ms = max(0, int(round(args.buffer_seconds * 1000)))

    scanner = launch_scanner(args, map_path, status_path)
    try:
        wait_for_initial_buffer(status_path, target_ms, args.poll_seconds)
        vlc_process = launch_vlc(args, rc_port, map_path, status_path)
        rc = connect_rc(rc_port, retries=30, delay_seconds=0.5)
        return guard_playback(args, scanner, vlc_process, rc, status_path, target_ms)
    except Exception as exc:
        print(f"nsfw_vlc_guard: {exc}", file=sys.stderr)
        if scanner.poll() is None:
            scanner.terminate()
            scanner.wait()
        return 1


if __name__ == "__main__":
    sys.exit(main())
