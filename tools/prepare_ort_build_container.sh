#!/usr/bin/env bash
# Phase 1: Prepare Docker container with all deps for multi-EP ORT build.
# Uses ubuntu:24.04 base + apt-installed CUDA (lighter than nvidia/cuda:devel).
set -euo pipefail

CONTAINER_NAME="ort-prep"
IMAGE_TAG="icop-ort-base:latest"

echo "=== Phase 1: Prepare build container ==="

docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

docker run -d \
    --name "${CONTAINER_NAME}" \
    ubuntu:24.04 \
    sleep infinity

# System deps
echo "--- System build deps ---"
docker exec "${CONTAINER_NAME}" bash -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq \
        build-essential cmake ninja-build \
        python3 python3-pip python3-venv \
        wget git lsb-release
    pip3 install --break-system-packages -q numpy pyyaml typing_extensions
'

# CUDA toolkit
echo "--- CUDA toolkit ---"
docker exec "${CONTAINER_NAME}" bash -c '
    export DEBIAN_FRONTEND=noninteractive
    wget -q https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404/x86_64/cuda-keyring_1.1-1_all.deb
    dpkg -i cuda-keyring_1.1-1_all.deb
    apt-get update -qq
    for pkg in cuda-toolkit-12-8 cuda-compiler-12-8; do
        apt-get install -y -qq "${pkg}" 2>/dev/null && break
    done
    rm -f cuda-keyring_1.1-1_all.deb
'

# cuDNN
echo "--- cuDNN ---"
docker exec "${CONTAINER_NAME}" bash -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get install -y -qq libcudnn9-dev-cuda-12 2>/dev/null || \
        echo "WARNING: cuDNN dev package not available"
'

# ROCm + MIGraphX
echo "--- ROCm + MIGraphX ---"
docker exec "${CONTAINER_NAME}" bash -c '
    export DEBIAN_FRONTEND=noninteractive
    wget -q -O /tmp/rocm.gpg.key https://repo.radeon.com/rocm/rocm.gpg.key
    mkdir -p /etc/apt/keyrings
    tee /etc/apt/keyrings/rocm.asc >/dev/null < /tmp/rocm.gpg.key
    echo "deb [signed-by=/etc/apt/keyrings/rocm.asc] https://repo.radeon.com/rocm/apt/6.2 noble main" \
        > /etc/apt/sources.list.d/rocm.list
    printf "Package: *\nPin: origin repo.radeon.com\nPin-Priority: 1001\n" \
        > /etc/apt/preferences.d/rocm-pin
    apt-get update -qq
    apt-get install -y -qq --allow-downgrades rocm-dev migraphx-dev 2>/dev/null || \
        echo "WARNING: ROCm/MIGraphX installation failed (non-fatal)"
'

# Copy build script
echo "--- Copying build script ---"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
docker cp "${SCRIPT_DIR}/build_ort_multi_ep.sh" "${CONTAINER_NAME}:/tmp/build_ort_multi_ep.sh"

# Commit
echo "--- Committing as ${IMAGE_TAG} ---"
docker commit "${CONTAINER_NAME}" "${IMAGE_TAG}"

docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

echo ""
echo "=== Phase 1 complete ==="
echo "Base image: ${IMAGE_TAG}"
echo "Size: $(docker image inspect ${IMAGE_TAG} --format='{{.Size}}' | numfmt --to=iec)"
echo ""
echo "Next: bash tools/build_ort_in_container.sh"
