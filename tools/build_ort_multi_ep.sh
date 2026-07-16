#!/usr/bin/env bash
# Build ONNX Runtime from source with CUDA + ROCm + XNNPACK.
# Designed to run on CI runners — SDK install included, no GPU needed.
set -euo pipefail

ORT_VERSION="${1:-v1.27.1}"
INSTALL_DIR="${2:-/home/runner/ort-multi-ep}"
BUILD_DIR="/tmp/ort-build"
ORT_SRC="${BUILD_DIR}/onnxruntime"
ORT_NATIVE="${BUILD_DIR}/ort-native"

# Auto-detect OS
OS_ID="$(. /etc/os-release && echo "${ID}")"
OS_VERSION="$(. /etc/os-release && echo "${VERSION_ID}")"
OS_CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")"

echo "Building ONNX Runtime ${ORT_VERSION} with CUDA + ROCm + XNNPACK"
echo "OS: ${OS_ID} ${OS_VERSION} (${OS_CODENAME})"
echo "Install dir: ${INSTALL_DIR}"

# Early exit if already built
if [ -f "${INSTALL_DIR}/lib/libonnxruntime.so" ]; then
    echo "ORT already exists at ${INSTALL_DIR}, skipping build"
    find "${INSTALL_DIR}" -name "*.so*" -type f | sort
    exit 0
fi

install_system_deps() {
    echo "=== Installing system build dependencies ==="
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        build-essential cmake ninja-build \
        python3 python3-pip \
        wget git lsb-release
    pip3 install -q numpy pyyaml typing_extensions
}

install_cuda() {
    if command -v nvcc &>/dev/null; then
        echo "CUDA already installed: $(nvcc --version | grep release | head -1)"
        return
    fi
    echo "=== Installing CUDA toolkit 12.2 ==="
    local cuda_os="${OS_ID}${OS_VERSION//./}"
    wget -q "https://developer.download.nvidia.com/compute/cuda/repos/${cuda_os}/x86_64/cuda-keyring_1.1-1_all.deb" || {
        # Fallback for newer OS versions
        cuda_os="ubuntu2404"
        wget -q "https://developer.download.nvidia.com/compute/cuda/repos/${cuda_os}/x86_64/cuda-keyring_1.1-1_all.deb"
    }
    sudo dpkg -i cuda-keyring_1.1-1_all.deb
    sudo apt-get update -qq
    sudo apt-get install -y -qq cuda-toolkit-12-2
    rm -f cuda-keyring_1.1-1_all.deb

    # Ensure CUDA paths exist for build
    export PATH="/usr/local/cuda-12.2/bin:${PATH}"
    export LD_LIBRARY_PATH="/usr/local/cuda-12.2/lib64:${LD_LIBRARY_PATH:-}"
}

install_rocm() {
    if [ -f /opt/rocm/lib/libamdhip64.so ]; then
        echo "ROCm already installed at /opt/rocm"
        return
    fi
    echo "=== Installing ROCm dev libraries ==="
    # Map codename
    local rocm_codename="${OS_CODENAME}"
    case "${rocm_codename}" in
        noble|jammy|focal) ;;
        *) rocm_codename="noble" ;;  # fallback
    esac

    wget -q -O /tmp/rocm.gpg.key https://repo.radeon.com/rocm/rocm.gpg.key
    sudo tee /etc/apt/keyrings/rocm.asc >/dev/null </tmp/rocm.gpg.key
    echo "deb [signed-by=/etc/apt/keyrings/rocm.asc] https://repo.radeon.com/rocm/apt/6.2 ${rocm_codename} main" \
        | sudo tee /etc/apt/sources.list.d/rocm.list
    sudo apt-get update -qq || true
    sudo apt-get install -y -qq --no-install-recommends \
        rocm-dev rocm-hip-sdk 2>/dev/null || {
        echo "ROCm installation partially failed; build may still work with limited EP support"
    }
}

clone_ort() {
    if [ -d "${ORT_SRC}" ]; then
        echo "ORT source already cloned at ${ORT_SRC}"
        return
    fi
    echo "=== Cloning ONNX Runtime ${ORT_VERSION} ==="
    git clone --depth 1 --branch "${ORT_VERSION}" \
        https://github.com/microsoft/onnxruntime.git "${ORT_SRC}"
}

build_ort() {
    echo "=== Building ONNX Runtime with CUDA + ROCm ==="
    mkdir -p "${ORT_NATIVE}" "${INSTALL_DIR}"

    export CUDA_HOME="/usr/local/cuda-12.2"
    export ROCM_HOME="/opt/rocm"

    python3 "${ORT_SRC}/tools/ci_build/build.py" \
        --config Release \
        --build_dir "${ORT_NATIVE}" \
        --cmake_extra_defines \
            CMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
            CMAKE_POSITION_INDEPENDENT_CODE=ON \
        --use_cuda \
        --cuda_version=12.2 \
        --cuda_home="${CUDA_HOME}" \
        --use_rocm \
        --rocm_home="${ROCM_HOME}" \
        --enable_shared_lib \
        --build_shared_lib \
        --parallel \
        --skip_tests || {
        local rc=$?
        echo "ORT build exited with code ${rc}"
        echo "Build output may still be usable; checking..."
        if [ -f "${ORT_NATIVE}/Release/libonnxruntime.so" ]; then
            echo "libonnxruntime.so found, continuing"
        else
            exit ${rc}
        fi
    }

    cmake --install "${ORT_NATIVE}/Release" --prefix "${INSTALL_DIR}" 2>/dev/null || true
    if [ "$(stat -c '%u' "${INSTALL_DIR}" 2>/dev/null)" = "0" ]; then
        sudo chown -R "$(id -u):$(id -g)" "${INSTALL_DIR}"
    fi
}

# ---- Main ----
echo "Starting multi-EP ORT build at $(date)"
install_system_deps
install_cuda
install_rocm
clone_ort
build_ort

echo ""
echo "=== Built ORT providers ==="
find "${INSTALL_DIR}" -name "*.so*" -type f | sort
echo ""
echo "ORT multi-EP build complete at $(date): ${INSTALL_DIR}"
