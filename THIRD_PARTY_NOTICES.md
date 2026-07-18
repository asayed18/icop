# Third-Party Notices

The source repository does not vendor ONNX models, ONNX Runtime binaries,
FFmpeg binaries, VLC binaries, or GoogleTest source. CMake may download them
for local builds and testing. Their original licenses and terms apply.

## Runtime and Build Dependencies

| Component | Use | License/source |
| --- | --- | --- |
| VLC media player | Plugin host and compatibility headers | GPL/LGPL family; see [VideoLAN legal information](https://www.videolan.org/legal.html) and file-level notices |
| ONNX Runtime | Model inference runtime | [MIT](https://github.com/microsoft/onnxruntime/blob/main/LICENSE) |
| Microsoft DirectML | Windows DirectML sidecar runtime | [Microsoft.AI.DirectML redistributable](https://www.nuget.org/packages/Microsoft.AI.DirectML) |
| GoogleTest | Test framework | [BSD-3-Clause](https://github.com/google/googletest/blob/main/LICENSE) |
| FFmpeg | Benchmark and test fixture tooling | See [FFmpeg legal information](https://ffmpeg.org/legal.html) and the selected build configuration |

## Downloaded Models

| Profile | Upstream | Declared license |
| --- | --- | --- |
| Marqo | [Marqo/nsfw-image-detection-384](https://huggingface.co/Marqo/nsfw-image-detection-384) | Apache-2.0 |
| AdamCodd | [AdamCodd/vit-base-nsfw-detector](https://huggingface.co/AdamCodd/vit-base-nsfw-detector) | Apache-2.0 |
| Falconsai community ONNX | [onnx-community/nsfw_image_detection-ONNX](https://huggingface.co/onnx-community/nsfw_image_detection-ONNX) | No license metadata was declared when this notice was prepared; verify upstream terms before redistribution |
| Falconsai optional | [Falconsai/nsfw_image_detection_26](https://huggingface.co/Falconsai/nsfw_image_detection_26) | Apache-2.0 |
| Legacy | [iola1999/nsfw-detect-onnx](https://github.com/iola1999/nsfw-detect-onnx) | MIT |

Model licenses, datasets, and usage restrictions can change independently of
this project. Anyone redistributing binary packages containing models must
review and satisfy the current upstream terms. This is why the initial public
repository publication is source-only.
