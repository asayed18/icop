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
    echo "=== Installing CUDA toolkit ==="
    local cuda_os="${OS_ID}${OS_VERSION//./}"
    # Map known OS versions for CUDA repos
    case "${cuda_os}" in
        ubuntu2404|ubuntu2204|ubuntu2004) ;;
        ubuntu2510) cuda_os="ubuntu2404" ;;  # fallback
        *) cuda_os="ubuntu2404" ;;
    esac
    if ! wget -q "https://developer.download.nvidia.com/compute/cuda/repos/${cuda_os}/x86_64/cuda-keyring_1.1-1_all.deb"; then
        echo "WARNING: Could not download CUDA keyring, continuing without CUDA"
        return
    fi
    sudo dpkg -i cuda-keyring_1.1-1_all.deb
    sudo apt-get update -qq
    # Try multiple CUDA package names (varies by Ubuntu version and repo)
    local cuda_installed=false
    for pkg in cuda-toolkit-12-8 cuda-toolkit-12-6 cuda-toolkit-12-5 \
               cuda-toolkit-12 cuda-toolkit cuda-compiler-12-2; do
        if sudo apt-get install -y -qq "${pkg}" 2>/dev/null; then
            echo "Installed CUDA via package: ${pkg}"
            cuda_installed=true
            break
        fi
    done
    rm -f cuda-keyring_1.1-1_all.deb

    if ! ${cuda_installed}; then
        echo "WARNING: CUDA toolkit could not be installed, continuing without CUDA"
        echo "The ORT build will only have ROCm + XNNPACK support"
        return
    fi

    local cuda_ver=""
    # Try to get full version from nvcc first (e.g. 12.8, not just 12)
    local nvcc_bin
    nvcc_bin="$(command -v nvcc 2>/dev/null || find /usr/local/cuda-* /usr/local/cuda -name nvcc -type f 2>/dev/null | head -1)"
    if [ -n "${nvcc_bin}" ]; then
        cuda_ver="$("${nvcc_bin}" --version | grep -oP 'release \K[\d.]+')"
    fi
    if [ -z "${cuda_ver}" ]; then
        echo "WARNING: CUDA version auto-detection failed"
        return
    fi
    export CUDA_HOME="/usr/local/cuda-${cuda_ver}"
    export CUDA_VERSION="${cuda_ver}"
    export PATH="${CUDA_HOME}/bin:${PATH}"
    export LD_LIBRARY_PATH="${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"
    echo "CUDA ${cuda_ver} installed at ${CUDA_HOME}"

    # Install cuDNN for CUDA (needed by ORT build.py --cudnn_home)
    echo "=== Installing cuDNN ==="
    sudo apt-get install -y -qq libcudnn9-dev-cuda-12 2>/dev/null || {
        echo "WARNING: cuDNN dev package not available, continuing"
    }
    export CUDNN_HOME="/usr"
    echo "cuDNN home: ${CUDNN_HOME}"
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

    if ! wget -q -O /tmp/rocm.gpg.key https://repo.radeon.com/rocm/rocm.gpg.key; then
        echo "WARNING: Could not download ROCm GPG key, continuing without ROCm"
        return
    fi
    sudo mkdir -p /etc/apt/keyrings
    sudo tee /etc/apt/keyrings/rocm.asc >/dev/null </tmp/rocm.gpg.key
    local rocm_ver="6.2"
    echo "deb [signed-by=/etc/apt/keyrings/rocm.asc] https://repo.radeon.com/rocm/apt/${rocm_ver} ${rocm_codename} main" \
        | sudo tee /etc/apt/sources.list.d/rocm.list

    # Pin ROCm repo to priority 1001 — Ubuntu 24.04 universe provides hipcc/rocm-cmake
    # with higher version numbers that conflict with ROCm 6.2 pinned deps.
    printf "Package: *\nPin: origin repo.radeon.com\nPin-Priority: 1001\n" \
        | sudo tee /etc/apt/preferences.d/rocm-pin >/dev/null

    sudo apt-get update -qq || true
    sudo apt-get install -y -qq --allow-downgrades rocm-dev migraphx-dev || {
        local rc=$?
        echo "ROCm installation failed (exit ${rc}); continuing with limited EP support"
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
    echo "=== Building ONNX Runtime with available EPs ==="
    mkdir -p "${ORT_NATIVE}" "${INSTALL_DIR}"

    local ep_flags=()
    local ep_flags_str=""

    # CUDA
    if [ -n "${CUDA_HOME:-}" ] && [ -f "${CUDA_HOME}/bin/nvcc" ]; then
        local cuda_ver="${CUDA_VERSION:-12}"
        local cudnn_home="${CUDNN_HOME:-}"
        if [ -n "${cudnn_home}" ]; then
            ep_flags+=(--use_cuda --cuda_version="${cuda_ver}" --cuda_home="${CUDA_HOME}" --cudnn_home="${cudnn_home}")
        else
            # Let build.py auto-detect cuDNN via env or defaults
            ep_flags+=(--use_cuda --cuda_version="${cuda_ver}" --cuda_home="${CUDA_HOME}" --cudnn_home=/usr)
        fi
        ep_flags_str+="CUDA "
        echo "CUDA EP enabled: ${CUDA_HOME}"
    else
        echo "CUDA EP disabled (CUDA not available)"
    fi

    # AMD MIGraphX (replaces the old ROCm EP in ORT >= 1.25)
    if [ -d /opt/rocm ] && ls /opt/rocm/lib/libmigraphx*.so* &>/dev/null; then
        ep_flags+=(--use_migraphx --migraphx_home=/opt/rocm)
        ep_flags_str+="MIGraphX "
        echo "AMD MIGraphX EP enabled: /opt/rocm"
    elif [ -d /opt/rocm ] && ls /opt/rocm/lib/libamdhip64.so* &>/dev/null; then
        ep_flags+=(--use_rocm --rocm_home=/opt/rocm)
        ep_flags_str+="ROCm "
        echo "ROCm EP enabled (legacy): /opt/rocm"
    else
        echo "AMD EP disabled (ROCm/MIGraphX not available)"
    fi

    if [ ${#ep_flags[@]} -eq 0 ]; then
        echo "WARNING: No GPU EPs available, building with CPU + XNNPACK only"
    fi

    python3 "${ORT_SRC}/tools/ci_build/build.py" \
        --config Release \
        --build_dir "${ORT_NATIVE}" \
        --cmake_extra_defines \
            CMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
            CMAKE_POSITION_INDEPENDENT_CODE=ON \
        "${ep_flags[@]}" \
        --build_shared_lib \
        --allow_running_as_root \
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
