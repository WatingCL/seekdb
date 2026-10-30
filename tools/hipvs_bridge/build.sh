#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}
# Radeon PRO W7900 / W7800 / RX 7900 (Navi 31) and the other RDNA3 parts.
OFFLOAD_ARCHS=${SEEKDB_CUVS_OFFLOAD_ARCHS:-gfx1100 gfx1101 gfx1102}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_ROOT/lib/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_ROOT/lib" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"
OBJ_DIR=$(mktemp -d)
trap 'rm -rf "$OBJ_DIR"' EXIT

ARCH_FLAGS=()
for arch in $OFFLOAD_ARCHS; do ARCH_FLAGS+=("--offload-arch=$arch"); done

# Device kernels of the graph optimisation (HIP).
"$HIPCC" \
  -c \
  -fPIC \
  -O3 \
  -std=c++17 \
  -x hip \
  "${ARCH_FLAGS[@]}" \
  -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_cuvs_graph.cu" \
  -o "$OBJ_DIR/seekdb_cuvs_graph.o"

"$HIPCC" \
  -c \
  -fPIC \
  -O2 \
  -D__HIP_PLATFORM_AMD__ \
  -I"$HIPVS_ROOT/include" \
  -I"$ROCM_ROOT/include" \
  "$SCRIPT_DIR/seekdb_cuvs_bridge.c" \
  -o "$OBJ_DIR/seekdb_cuvs_bridge.o"

"$HIPCC" \
  -shared \
  -fPIC \
  --hip-link \
  "$OBJ_DIR/seekdb_cuvs_bridge.o" \
  "$OBJ_DIR/seekdb_cuvs_graph.o" \
  -L"$HIPVS_ROOT/lib" \
  -Wl,-soname,libseekdb_cuvs_bridge.so \
  -Wl,-rpath,"$HIPVS_ROOT/lib:$ROCM_ROOT/lib" \
  -lcuvs_c \
  -o "$OUTPUT"

echo "built $OUTPUT"
