#!/usr/bin/env bash
# Build multi-EP ORT inside a Docker container for glibc compatibility.
# Usage: bash tools/build_ort_multi_ep_docker.sh
set -euo pipefail

ORT_VERSION="${1:-v1.27.1}"
IMAGE_TAG="icop-ort-builder:latest"
OUTPUT_DIR="${2:-/tmp/ort-multi-ep-output}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=== Building multi-EP ORT ${ORT_VERSION} in Docker ==="
echo "Output: ${OUTPUT_DIR}"

mkdir -p "${OUTPUT_DIR}"

docker build -t "${IMAGE_TAG}" \
    --build-arg ORT_VERSION="${ORT_VERSION}" \
    -f - "${PROJECT_DIR}" << 'DOCKERFILE'
ARG ORT_VERSION=v1.27.1

FROM nvidia/cuda:12.8.0-devel-ubuntu24.04 AS builder

ARG ORT_VERSION
ARG DEBIAN_FRONTEND=noninteractive
ENV CUDA_HOME=/usr/local/cuda
ENV PATH="${CUDA_HOME}/bin:${PATH}"
ENV LD_LIBRARY_PATH="${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

RUN apt-get update -qq && \
    apt-get install -y -qq \
        build-essential cmake ninja-build \
        python3 python3-pip python3-venv \
        wget git lsb-release && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

RUN pip3 install --break-system-packages -q numpy pyyaml typing_extensions

RUN echo "=== Installing cuDNN ===" && \
    apt-get update -qq && \
    apt-get install -y -qq libcudnn9-dev-cuda-12 2>/dev/null || \
    echo "WARNING: cuDNN dev package not available"

ENV CUDNN_HOME=/usr

# Inject ROCm repo and install MIGraphX
RUN echo "=== Installing ROCm + MIGraphX ===" && \
    wget -q -O /tmp/rocm.gpg.key https://repo.radeon.com/rocm/rocm.gpg.key && \
    mkdir -p /etc/apt/keyrings && \
    tee /etc/apt/keyrings/rocm.asc >/dev/null < /tmp/rocm.gpg.key && \
    echo "deb [signed-by=/etc/apt/keyrings/rocm.asc] https://repo.radeon.com/rocm/apt/6.2 noble main" \
        > /etc/apt/sources.list.d/rocm.list && \
    printf "Package: *\nPin: origin repo.radeon.com\nPin-Priority: 1001\n" \
        > /etc/apt/preferences.d/rocm-pin && \
    apt-get update -qq && \
    apt-get install -y -qq --allow-downgrades rocm-dev migraphx-dev 2>/dev/null || \
    echo "WARNING: ROCm/MIGraphX installation failed"

COPY tools/build_ort_multi_ep.sh /tmp/build_ort_multi_ep.sh
RUN bash /tmp/build_ort_multi_ep.sh "${ORT_VERSION}" /opt/ort-multi-ep

FROM scratch AS artifact
COPY --from=builder /opt/ort-multi-ep /opt/ort-multi-ep
DOCKERFILE

echo "=== Extracting artifact ==="
CONTAINER_ID=$(docker create "${IMAGE_TAG}")
docker cp "${CONTAINER_ID}:/opt/ort-multi-ep" - > "${OUTPUT_DIR}/ort-multi-ep.tar"
docker rm "${CONTAINER_ID}"

echo "=== Artifact contents ==="
tar tf "${OUTPUT_DIR}/ort-multi-ep.tar" | grep '\.so' | sort
echo ""
echo "=== Archive size ==="
ls -lh "${OUTPUT_DIR}/ort-multi-ep.tar"
echo ""
echo "Build complete: ${OUTPUT_DIR}/ort-multi-ep.tar"
