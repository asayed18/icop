#!/usr/bin/env python3
"""Export Falconsai/nsfw_image_detection to an ONNX model.

The exporter downloads the base Hugging Face snapshot if needed, exports a
fixed-shape batch-1 ONNX graph, and tries to quantize it for faster runtime
inference. If quantization is not available, it falls back to the FP32 export.
"""

from __future__ import annotations

import argparse
import os
import shutil
import tempfile
from pathlib import Path

import onnx
import torch
from huggingface_hub import snapshot_download
from onnxruntime.quantization import QuantType, quantize_dynamic
from transformers import AutoModelForImageClassification


class OnnxExportWrapper(torch.nn.Module):
    def __init__(self, model: torch.nn.Module) -> None:
        super().__init__()
        self.model = model

    def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
        return self.model(pixel_values=pixel_values).logits


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export Falconsai/nsfw_image_detection to ONNX"
    )
    parser.add_argument(
        "--repo",
        default="Falconsai/nsfw_image_detection",
        help="Hugging Face repository id to export",
    )
    parser.add_argument(
        "--snapshot-dir",
        default="",
        help="Directory containing a downloaded model snapshot",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Destination ONNX file path",
    )
    parser.add_argument(
        "--no-quantize",
        action="store_true",
        help="Skip dynamic quantization and keep the FP32 export",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=17,
        help="ONNX opset version to export",
    )
    return parser.parse_args()


def resolve_snapshot_dir(repo_id: str, snapshot_dir: str, output_path: Path) -> Path:
    if snapshot_dir:
        candidate = Path(snapshot_dir)
        if (candidate / "config.json").exists():
            return candidate

    cache_dir = output_path.parent / "falconsai_base_model"
    cache_dir.mkdir(parents=True, exist_ok=True)
    token = os.environ.get("HF_TOKEN") or os.environ.get("HUGGINGFACE_HUB_TOKEN")
    snapshot_download(repo_id=repo_id, local_dir=str(cache_dir), token=token)
    return cache_dir


def export_onnx(snapshot_path: Path, output_path: Path, opset_version: int,
                quantize: bool) -> None:
    model = AutoModelForImageClassification.from_pretrained(snapshot_path)
    model.eval()

    wrapped = OnnxExportWrapper(model)
    wrapped.eval()

    temp_fp32 = output_path.with_suffix(".fp32.onnx")
    dummy_input = torch.randn(1, 3, 224, 224, dtype=torch.float32)

    output_path.unlink(missing_ok=True)
    temp_fp32.unlink(missing_ok=True)

    torch.onnx.export(
        wrapped,
        (dummy_input,),
        temp_fp32.as_posix(),
        input_names=["pixel_values"],
        output_names=["logits"],
        opset_version=opset_version,
        dynamo=False,
        do_constant_folding=True,
        verbose=False,
    )

    onnx.checker.check_model(onnx.load(temp_fp32.as_posix()))

    if quantize:
        try:
            quantize_dynamic(
                temp_fp32.as_posix(),
                output_path.as_posix(),
                weight_type=QuantType.QInt8,
            )
            onnx.checker.check_model(onnx.load(output_path.as_posix()))
            temp_fp32.unlink(missing_ok=True)
            return
        except Exception as exc:  # pragma: no cover - fallback path
            print(f"Quantization failed, keeping FP32 export: {exc}")

    output_path.unlink(missing_ok=True)
    shutil.move(temp_fp32.as_posix(), output_path.as_posix())


def main() -> int:
    args = parse_args()
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    snapshot_path = resolve_snapshot_dir(args.repo, args.snapshot_dir, output_path)
    export_onnx(snapshot_path, output_path, args.opset, not args.no_quantize)

    print(f"Exported ONNX model to {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
