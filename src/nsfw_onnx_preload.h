#ifndef NSFW_ONNX_PRELOAD_H
#define NSFW_ONNX_PRELOAD_H

#include <string>

extern std::string s_gpu_ort_directory;

bool nsfw_plat_preload_cuda_runtime_libraries(void);
bool nsfw_plat_preload_rocm_runtime_libraries(void);

#endif
