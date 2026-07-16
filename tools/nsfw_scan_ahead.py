#!/usr/bin/env python3
import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
from pathlib import Path


class NsfwConfig(ctypes.Structure):
    _fields_ = [
        ("threshold", ctypes.c_float),
        ("model_width", ctypes.c_int),
        ("model_height", ctypes.c_int),
        ("model_path", ctypes.c_char_p),
    ]


class NsfwResult(ctypes.Structure):
    _fields_ = [
        ("is_nsfw", ctypes.c_int),
        ("score", ctypes.c_float),
        ("threshold", ctypes.c_float),
    ]


class DetectorCore:
    def __init__(self, dll_path: Path, model_path: str | None, threshold: float) -> None:
        self._dll_directories: list[object] = []
        for directory in runtime_search_directories(dll_path):
            if hasattr(os, "add_dll_directory"):
                try:
                    self._dll_directories.append(os.add_dll_directory(str(directory)))
                except OSError:
                    pass
        self._dll = ctypes.CDLL(str(dll_path))
        self._dll.nsfw_config_default.restype = NsfwConfig
        self._dll.nsfw_detector_create.argtypes = [ctypes.POINTER(NsfwConfig)]
        self._dll.nsfw_detector_create.restype = ctypes.c_void_p
        self._dll.nsfw_detector_destroy.argtypes = [ctypes.c_void_p]
        self._dll.nsfw_detector_destroy.restype = None
        self._dll.nsfw_detector_classify.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
        ]
        self._dll.nsfw_detector_classify.restype = NsfwResult

        config = self._dll.nsfw_config_default()
        config.threshold = threshold
        self.model_width = int(config.model_width)
        self.model_height = int(config.model_height)
        self._model_bytes = model_path.encode("utf-8") if model_path else None
        if self._model_bytes is not None:
            config.model_path = ctypes.c_char_p(self._model_bytes)
        self._detector = self._dll.nsfw_detector_create(ctypes.byref(config))
        if not self._detector:
            raise RuntimeError("failed to create NSFW detector")

    def classify(self, frame_bytes: bytes) -> NsfwResult:
        array_type = ctypes.c_uint8 * len(frame_bytes)
        frame_array = array_type.from_buffer_copy(frame_bytes)
        return self._dll.nsfw_detector_classify(
            self._detector,
            frame_array,
            self.model_width,
            self.model_height,
            3,
        )

    def close(self) -> None:
        if self._detector:
            self._dll.nsfw_detector_destroy(self._detector)
            self._detector = None


def find_default_core_dll(script_path: Path) -> Path:
    candidates = [
        script_path.parents[1] / "stage" / "plugins" / "video_filter" / "icop_core.dll",
        script_path.parents[1] / "build-ninja" / "icop_core.dll",
        script_path.with_name("icop_core.dll"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError("could not locate icop_core.dll")


def runtime_search_directories(dll_path: Path) -> list[Path]:
    repo_root = Path(__file__).resolve().parents[1]
    directories = [dll_path.parent]
    candidates = [
        repo_root / "stage" / "plugins" / "video_filter",
        repo_root / "vlc-portable" / "plugins" / "video_filter",
        repo_root / "build-ninja",
        repo_root / "build-ninja-gpu",
    ]

    for candidate in candidates:
        if candidate.exists():
            directories.append(candidate)
        if candidate.exists():
            for runtime_dll in candidate.rglob("onnxruntime.dll"):
                directories.append(runtime_dll.parent)

    unique: list[Path] = []
    seen: set[str] = set()
    for directory in directories:
        key = str(directory.resolve())
        if key not in seen and directory.exists():
            seen.add(key)
            unique.append(directory)
    return unique


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False, dir=path.parent) as handle:
        handle.write(text)
        temp_path = Path(handle.name)
    try:
        temp_path.replace(path)
    except OSError:
        try:
            path.write_text(text, encoding="utf-8")
        finally:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass


def write_decision_map(path: Path, ranges: list[tuple[int, int]]) -> None:
    lines = ["# nsfw decision map v1\n"]
    for start_ms, end_ms in ranges:
        lines.append(f"blocked {start_ms} {end_ms}\n")
    atomic_write(path, "".join(lines))


def write_status(path: Path, last_scanned_ms: int, duration_ms: int, done: bool, sample_fps: float) -> None:
    atomic_write(
        path,
        "\n".join(
            [
                "version=1",
                f"last_scanned_ms={last_scanned_ms}",
                f"duration_ms={duration_ms}",
                f"done={1 if done else 0}",
                f"sample_fps={sample_fps:.6f}",
            ]
        )
        + "\n",
    )


def ffprobe_duration_ms(input_path: Path) -> int:
    command = [
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(input_path),
    ]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    seconds = float(completed.stdout.strip() or "0")
    return int(round(seconds * 1000))


def scanner_command(input_path: Path, width: int, height: int, sample_fps: float) -> list[str]:
    return [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(input_path),
        "-an",
        "-sn",
        "-vf",
        f"fps={sample_fps},scale={width}:{height}:flags=bicubic",
        "-pix_fmt",
        "rgb24",
        "-f",
        "rawvideo",
        "-",
    ]


def update_ranges(
    ranges: list[tuple[int, int]],
    blocked: bool,
    interval_start_ms: int,
    interval_end_ms: int,
) -> None:
    if not blocked:
        return
    if ranges and interval_start_ms <= ranges[-1][1]:
        ranges[-1] = (ranges[-1][0], max(ranges[-1][1], interval_end_ms))
        return
    ranges.append((interval_start_ms, interval_end_ms))


def run_scan(args: argparse.Namespace) -> int:
    script_path = Path(__file__).resolve()
    if args.provider:
        os.environ["NSFW_ONNX_PROVIDER"] = args.provider
    core_dll = Path(args.core_dll) if args.core_dll else find_default_core_dll(script_path)
    detector = DetectorCore(core_dll, args.model_path, args.threshold)
    duration_ms = ffprobe_duration_ms(Path(args.input))
    interval_ms = 1000.0 / args.sample_fps
    hold_ms = max(0, int(round(args.hold_seconds * 1000)))
    ranges: list[tuple[int, int]] = []
    active_block_until_ms = 0
    last_scanned_ms = 0
    frame_size = detector.model_width * detector.model_height * 3
    processed_frames = 0

    process = subprocess.Popen(
        scanner_command(Path(args.input), detector.model_width, detector.model_height, args.sample_fps),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=os.environ.copy(),
    )

    try:
        while True:
            assert process.stdout is not None
            frame_bytes = process.stdout.read(frame_size)
            if not frame_bytes:
                break
            if len(frame_bytes) != frame_size:
                raise RuntimeError("truncated rawvideo frame from ffmpeg")

            result = detector.classify(frame_bytes)
            interval_start_ms = int(round(processed_frames * interval_ms))
            interval_end_ms = int(round((processed_frames + 1) * interval_ms))

            if result.is_nsfw:
                active_block_until_ms = max(active_block_until_ms, interval_end_ms + hold_ms)

            blocked = interval_start_ms < active_block_until_ms
            update_ranges(ranges, blocked, interval_start_ms, max(interval_end_ms, active_block_until_ms if blocked else interval_end_ms))
            last_scanned_ms = interval_end_ms
            processed_frames += 1

            if processed_frames == 1 or processed_frames % args.write_every == 0:
                write_decision_map(Path(args.output), ranges)
                write_status(Path(args.status), min(last_scanned_ms, duration_ms), duration_ms, False, args.sample_fps)

        stderr_text = ""
        if process.stderr is not None:
            stderr_text = process.stderr.read().decode("utf-8", errors="replace")
        return_code = process.wait()
        if return_code != 0:
            raise RuntimeError(f"ffmpeg failed with code {return_code}: {stderr_text.strip()}")

        write_decision_map(Path(args.output), ranges)
        write_status(Path(args.status), duration_ms if duration_ms > 0 else last_scanned_ms, duration_ms, True, args.sample_fps)
        return 0
    finally:
        detector.close()
        if process.poll() is None:
            process.kill()
            process.wait()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Scan a video ahead of playback and emit blocked time ranges.")
    parser.add_argument("--input", required=True, help="Input video path.")
    parser.add_argument("--output", required=True, help="Output decision map path.")
    parser.add_argument("--status", required=True, help="Output scan status path.")
    parser.add_argument("--core-dll", help="Path to icop_core.dll.")
    parser.add_argument("--model-path", help="Optional ONNX model path.")
    parser.add_argument("--provider", default="gpu", help="ONNX provider preference.")
    parser.add_argument("--threshold", type=float, default=0.5, help="Detection threshold.")
    parser.add_argument("--sample-fps", type=float, default=3.0, help="Sampling rate for ahead-of-time scanning.")
    parser.add_argument("--hold-seconds", type=float, default=0.4, help="Extra blocked time after a positive sample.")
    parser.add_argument("--write-every", type=int, default=3, help="Write output files every N processed samples.")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if args.sample_fps <= 0:
        parser.error("--sample-fps must be positive")
    if args.write_every <= 0:
        parser.error("--write-every must be positive")
    try:
        return run_scan(args)
    except Exception as exc:
        print(f"nsfw_scan_ahead: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
