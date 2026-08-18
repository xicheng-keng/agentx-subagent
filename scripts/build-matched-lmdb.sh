#!/usr/bin/env bash
# build-matched-lmdb.sh -- build a liblmdb from the exact source tree that
# the Rust side (heed -> lmdb-master-sys) vendors, so the C subagent links
# against an LMDB build that is lock-file/ABI compatible with it.
#
# *** Why this exists (a real cross-process interop bug this test harness
# found) ***
#
# docs/design.md ch.2 shares config.lmdb/cache.lmdb between the C subagent
# (linked against whatever `liblmdb-dev` the OS package manager ships, e.g.
# Debian/Ubuntu's 0.9.31) and the Rust app (heed -> lmdb-master-sys, which
# always compiles its own vendored LMDB from source -- 0.9.70 as of this
# writing -- and offers no option to link the system library instead).
#
# These two LMDB builds are NOT wire-compatible for concurrent, *live*
# multi-process access to the same environment: when one process created
# (or holds an open handle on) an environment with one library build and a
# second process built against the other library build tries to attach to
# the same lock table while the first is still live, `mdb_env_open` fails
# with `MDB_VERSION_MISMATCH`. It was not visible in a quick single-process
# smoke test (the failure only shows up once the C daemon has a live open
# handle when the Rust side attaches), which is exactly the cross-process
# scenario docs/design.md 5.3 asks this harness to validate -- so scenarios
# (b)/(d)/(c) will intermittently or consistently fail with this error
# unless both sides are built from the same LMDB source.
#
# The fix here is to build the identical vendored source the Rust side uses
# and make the C subagent link *that* instead of the distro package. This
# script:
#   1. Runs `cargo build` for rust-app (if not already built) so Cargo
#      fetches/vendors lmdb-master-sys into the local registry cache.
#   2. Locates that vendored `lmdb/libraries/liblmdb` source tree.
#   3. Compiles it into a static lib + matching header under
#      $OUT_PREFIX (default: <repo>/.lmdb-matched).
#
# Usage:
#   scripts/build-matched-lmdb.sh [OUT_PREFIX]
#
# Then point cmake at it (no CMakeLists.txt changes needed, these are
# ordinary cache variables the existing find_path/find_library calls read):
#   cmake -S . -B build-it \
#     -DLMDB_INCLUDE_DIR=<OUT_PREFIX>/include \
#     -DLMDB_LIBRARY=<OUT_PREFIX>/lib/libmdb-matched.a
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_PREFIX="${1:-${REPO_ROOT}/.lmdb-matched}"

echo "build-matched-lmdb: ensuring rust-app is built so Cargo vendors lmdb-master-sys ..."
( cd "${REPO_ROOT}/rust-app" && cargo build --release >/dev/null )

CARGO_HOME="${CARGO_HOME:-${HOME}/.cargo}"
LMDB_SYS_DIR="$(find "${CARGO_HOME}/registry/src" -mindepth 2 -maxdepth 2 -type d -iname 'lmdb-master-sys-*' 2>/dev/null | sort -V | tail -1)"
if [[ -z "${LMDB_SYS_DIR}" ]]; then
  echo "build-matched-lmdb: could not find a vendored lmdb-master-sys source under ${CARGO_HOME}/registry/src" >&2
  echo "build-matched-lmdb: (rust-app/Cargo.lock pins the version; 'cargo build' above should have fetched it)" >&2
  exit 1
fi

LMDB_C_SRC="${LMDB_SYS_DIR}/lmdb/libraries/liblmdb"
if [[ ! -f "${LMDB_C_SRC}/mdb.c" ]]; then
  echo "build-matched-lmdb: ${LMDB_C_SRC}/mdb.c not found; lmdb-master-sys's vendored layout may have changed" >&2
  exit 1
fi

echo "build-matched-lmdb: building from ${LMDB_C_SRC}"
mkdir -p "${OUT_PREFIX}/include" "${OUT_PREFIX}/lib"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT
cp "${LMDB_C_SRC}/mdb.c" "${LMDB_C_SRC}/midl.c" "${LMDB_C_SRC}/lmdb.h" "${LMDB_C_SRC}/midl.h" "${WORKDIR}/"

(
  cd "${WORKDIR}"
  cc -O2 -fPIC -pthread -DNDEBUG -c mdb.c midl.c
  ar rcs libmdb-matched.a mdb.o midl.o
)

cp "${WORKDIR}/libmdb-matched.a" "${OUT_PREFIX}/lib/"
cp "${LMDB_C_SRC}/lmdb.h" "${OUT_PREFIX}/include/"

echo "build-matched-lmdb: done."
echo "  LMDB_INCLUDE_DIR=${OUT_PREFIX}/include"
echo "  LMDB_LIBRARY=${OUT_PREFIX}/lib/libmdb-matched.a"
