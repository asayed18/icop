#!/usr/bin/env bash
# Phase 3: Extract built ORT libraries from the container and package as .tar.gz.
set -euo pipefail

CONTAINER_NAME="ort-builder"
OUTPUT_DIR="${1:-/tmp/ort-multi-ep-output}"
ARCHIVE_NAME="ort-multi-ep-linux-x64.tar.gz"

echo "=== Phase 3: Extract ORT artifact ==="
mkdir -p "${OUTPUT_DIR}"

# Copy the entire install directory
echo "--- Copying from container ---"
docker cp "${CONTAINER_NAME}:/opt/ort-multi-ep" "${OUTPUT_DIR}/ort-multi-ep"

echo ""
echo "=== All .so files ==="
find "${OUTPUT_DIR}/ort-multi-ep" -name "*.so*" -type f | sort

echo ""
echo "=== Checking provider libs ==="
ls -lh "${OUTPUT_DIR}/ort-multi-ep/lib/"libonnxruntime_providers_*.so 2>/dev/null || \
    echo "No provider libs found!"

echo ""
echo "--- Creating archive ---"
tar czf "${OUTPUT_DIR}/${ARCHIVE_NAME}" \
    -C "${OUTPUT_DIR}" \
    ort-multi-ep/

echo ""
echo "=== Archive ==="
ls -lh "${OUTPUT_DIR}/${ARCHIVE_NAME}"

echo ""
echo "=== Size breakdown ==="
du -sh "${OUTPUT_DIR}/ort-multi-ep/lib/"*
