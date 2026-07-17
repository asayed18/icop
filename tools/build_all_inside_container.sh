#!/usr/bin/env bash
# Run inside the Docker container to install remaining deps and build ORT.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export CUDA_HOME="/usr/local/cuda-12.8"
export PATH="${CUDA_HOME}/bin:${PATH}"
export LD_LIBRARY_PATH="${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

ORT_VERSION="${1:-v1.27.1}"
INSTALL_DIR="${2:-/opt/ort-multi-ep}"

echo "=== Starting full ORT build at $(date) ==="

# 1. Fix any stale dpkg processes
echo "--- Fixing dpkg ---"
kill -9 4722 2>/dev/null || true
rm -f /var/lib/dpkg/lock-frontend /var/lib/apt/lists/lock /var/cache/apt/archives/lock /var/lib/dpkg/lock 2>/dev/null || true
dpkg --configure -a 2>/dev/null || true

# 2. Install cuDNN
echo "--- Installing cuDNN ---"
apt-get update -qq
apt-get install -y -qq libcudnn9-dev-cuda-12 2>/dev/null || echo "WARNING: cuDNN not available"
export CUDNN_HOME=/usr

# 3. Install ROCm + MIGraphX
echo "--- Installing ROCm + MIGraphX ---"
wget -q -O /tmp/rocm.gpg.key https://repo.radeon.com/rocm/rocm.gpg.key
mkdir -p /etc/apt/keyrings
tee /etc/apt/keyrings/rocm.asc >/dev/null < /tmp/rocm.gpg.key
echo "deb [signed-by=/etc/apt/keyrings/rocm.asc] https://repo.radeon.com/rocm/apt/6.2 noble main" \
    > /etc/apt/sources.list.d/rocm.list
printf "Package: *\nPin: origin repo.radeon.com\nPin-Priority: 1001\n" \
    > /etc/apt/preferences.d/rocm-pin
apt-get update -qq
apt-get install -y -qq --allow-downgrades --no-install-recommends \
    rocm-dev migraphx-dev 2>/dev/null || echo "WARNING: ROCm/MIGraphX installation failed"

# 4. Clean apt cache
apt-get clean
rm -rf /var/lib/apt/lists/*

echo "=== All dependencies installed at $(date) ==="
echo "CUDA: $(nvcc --version | grep release)"
echo "cuDNN: $(ls /usr/lib/x86_64-linux-gnu/libcudnn.so* 2>/dev/null | head -1)"
echo "ROCm: $(ls /opt/rocm/lib/libamdhip64.so 2>/dev/null | head -1)"
echo "MIGraphX: $(ls /opt/rocm/lib/libmigraphx*.so 2>/dev/null | head -1)"
echo "Disk: $(df -h / | tail -1)"

# 5. Now build ORT
echo ""
echo "=== Building ORT from source ==="
bash /opt/build_ort_multi_ep.sh "${ORT_VERSION}" "${INSTALL_DIR}"

echo ""
echo "=== Build complete at $(date) ==="
echo "Output:"
find "${INSTALL_DIR}" -name "*.so*" -type f | sort
echo ""
echo "Archive size:"
du -sh "${INSTALL_DIR}"
