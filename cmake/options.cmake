# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NSFW_BUILD_TESTS       "Build unit tests"       ON)
option(NSFW_BUILD_BENCHMARKS  "Build benchmark executable" ON)
option(NSFW_BUILD_VLC_MODULE  "Build VLC filter module" ON)
option(NSFW_INSTALL_VLC_PLUGIN "Install VLC module into a VLC tree" OFF)
option(NSFW_INSTALL_CUDA_RUNTIME
    "Install CUDA runtime DLLs with the VLC plugin" OFF)
option(NSFW_EXPORT_FALCONSAI_BASE_ONNX
    "Attempt to export Falconsai/nsfw_image_detection to ONNX when missing" ON)
option(NSFW_DOWNLOAD_MODELS
    "Download built-in ONNX models during configuration" ON)
option(NSFW_GPU_RUNTIME
    "Download the GPU-enabled ONNX Runtime binary and stage GPU provider libraries" OFF)
set(NSFW_CUDA_VERSION "" CACHE STRING
    "CUDA major version for GPU ONNX Runtime (auto-detected if empty)")
