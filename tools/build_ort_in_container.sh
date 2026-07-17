#!/usr/bin/env bash
# Phase 2: Run the multi-EP ORT build inside the prepared container.
# Monitor with: docker logs -f ort-builder
set -euo pipefail

CONTAINER_NAME="ort-builder"
IMAGE_TAG="icop-ort-base:latest"
ORT_VERSION="${1:-v1.27.1}"
OUTPUT_DIR="${2:-/tmp/ort-multi-ep-output}"

echo "=== Phase 2: Build multi-EP ORT in container ==="
echo "ORT version: ${ORT_VERSION}"
echo "Output dir: ${OUTPUT_DIR}"
echo ""
echo "Monitor with: docker logs -f ${CONTAINER_NAME}"
echo ""

mkdir -p "${OUTPUT_DIR}"

# Remove any previous container with same name
docker rm -f "${CONTAINER_NAME}" 2>/dev/null || true

# Run the build in a detached container
docker run -d \
    --name "${CONTAINER_NAME}" \
    "${IMAGE_TAG}" \
    bash /tmp/build_ort_multi_ep.sh "${ORT_VERSION}" /opt/ort-multi-ep

echo "Container ${CONTAINER_NAME} started."
echo "The build will take 2-6 hours."
echo ""
echo "  # Check progress:"
echo "  docker logs -f ${CONTAINER_NAME}"
echo ""
echo "  # After completion, extract:"
echo "  bash tools/extract_ort_from_container.sh"
