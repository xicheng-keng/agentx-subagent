#!/usr/bin/env bash
# gen_proto.sh — regenerate src/generated_pb/agentx_ipc.pb.{c,h} from
# proto/agentx_ipc.proto + proto/agentx_ipc.options using the vendored
# nanopb generator under third_party/nanopb.
#
# This script only regenerates the C (nanopb) side. The Rust app's prost
# build is handled by its own build.rs and is NOT touched here.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROTO_DIR="${ROOT_DIR}/proto"
NANOPB_DIR="${ROOT_DIR}/third_party/nanopb"
OUT_DIR="${ROOT_DIR}/src/generated_pb"

command -v protoc >/dev/null 2>&1 || { echo "error: protoc not found in PATH" >&2; exit 1; }

mkdir -p "${OUT_DIR}"

PYTHON_BIN="${PYTHON_BIN:-python3}"

"${PYTHON_BIN}" "${NANOPB_DIR}/generator/nanopb_generator.py" \
    -I "${PROTO_DIR}" \
    -D "${OUT_DIR}" \
    -f "${PROTO_DIR}/agentx_ipc.options" \
    "${PROTO_DIR}/agentx_ipc.proto"

echo "Generated ${OUT_DIR}/agentx_ipc.pb.c and .h"
