#!/usr/bin/env bash
# common.sh -- shared helpers for the agentx-subagent cross-process
# integration scenarios (docs/design.md ch.5.3).
#
# Sourced (not executed) by every tests/scenario_*.sh script. Provides:
#   - repo root / build artifact discovery (no hardcoded /home/user paths)
#   - PASS/FAIL bookkeeping with a non-zero exit on any failure
#   - scratch directory + high port allocation (16100-16300)
#   - snmpd / agentx-subagent process lifecycle with trap-based cleanup
#   - small numeric helpers (percentiles) shared by the load scenarios
#
# Every scenario script must:
#   set -euo pipefail
#   source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
#   itest_init "<scenario-name>"
#   trap itest_cleanup EXIT
# and call itest_pass/itest_fail as it goes, ending with itest_report.

# shellcheck disable=SC2034  # some of these are used only by sourcing scripts

# --- repo layout ---------------------------------------------------------

ITEST_TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ITEST_REPO_ROOT="$(cd "${ITEST_TESTS_DIR}/.." && pwd)"

# The build directory is the caller's own (per the task's "use your own build
# dir" instruction); override with ITEST_BUILD_DIR if a script needs a
# different one. scripts/run-integration.sh sets this once for every scenario.
: "${ITEST_BUILD_DIR:=${ITEST_REPO_ROOT}/build-it}"

ITEST_SUBAGENT_BIN="${ITEST_BUILD_DIR}/agentx-subagent"
ITEST_STORAGE_CLI="${ITEST_BUILD_DIR}/storage_cli"
ITEST_RUST_RELEASE_DIR="${ITEST_REPO_ROOT}/rust-app/target/release"
ITEST_TELEMETRY_BIN="${ITEST_RUST_RELEASE_DIR}/telemetry"
ITEST_LOADGEN_BIN="${ITEST_RUST_RELEASE_DIR}/loadgen"
ITEST_IT_HELPER_BIN="${ITEST_TESTS_DIR}/rust_helpers/target/release/it_helper"

# --- bookkeeping -----------------------------------------------------------

ITEST_NAME=""
ITEST_SCRATCH=""
ITEST_PASS_COUNT=0
ITEST_FAIL_COUNT=0
ITEST_PIDS=()          # background PIDs this scenario started, killed on exit
ITEST_SKIPPED=0

itest_log() { printf '[%s] %s\n' "${ITEST_NAME}" "$*"; }

itest_pass() {
  ITEST_PASS_COUNT=$((ITEST_PASS_COUNT + 1))
  printf '[%s] PASS: %s\n' "${ITEST_NAME}" "$*"
}

itest_fail() {
  ITEST_FAIL_COUNT=$((ITEST_FAIL_COUNT + 1))
  printf '[%s] FAIL: %s\n' "${ITEST_NAME}" "$*" >&2
}

# Print a loud SKIP banner and exit 0 immediately. Use this only for
# environment-capability gaps (missing privileges, missing binaries) --
# never to paper over an assertion failure.
itest_skip() {
  ITEST_SKIPPED=1
  printf '[%s] SKIP: %s\n' "${ITEST_NAME}" "$*"
  exit 0
}

# Call once at the top of every scenario, right after sourcing this file.
itest_init() {
  ITEST_NAME="$1"
  ITEST_SCRATCH="$(mktemp -d "/tmp/agentx-it-${ITEST_NAME}.XXXXXX")"
  itest_log "scratch dir: ${ITEST_SCRATCH}"
}

# Registered via `trap itest_cleanup EXIT` by every scenario script.
itest_cleanup() {
  local rc=$?
  local pid
  # Union of the in-shell array (fast path, covers the common case of a
  # direct, non-subshell call) and the on-disk record (authoritative --
  # covers every itest_start_subagent/itest_start_snmpd call made through a
  # `$(...)` command substitution, which runs in a subshell whose ITEST_PIDS
  # appends never reach this, the parent shell's, array).
  local all_pids=("${ITEST_PIDS[@]:-}")
  if [[ -n "${ITEST_SCRATCH:-}" && -f "${ITEST_SCRATCH}/.tracked_pids" ]]; then
    while IFS= read -r pid; do
      [[ -n "${pid}" ]] && all_pids+=("${pid}")
    done < "${ITEST_SCRATCH}/.tracked_pids"
  fi
  for pid in "${all_pids[@]:-}"; do
    [[ -n "${pid}" ]] || continue
    if kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      wait "${pid}" 2>/dev/null || true
    fi
  done
  # Keep the scratch directory when there is something to look at: a failed
  # scenario's logs are the whole point of having them, and deleting them on
  # the way out leaves a CI failure with no evidence attached. A pass (or a
  # skip) still cleans up, so repeated local runs do not accumulate.
  if [[ -n "${ITEST_SCRATCH}" && -d "${ITEST_SCRATCH}" ]]; then
    if [[ ${rc} -ne 0 && ${ITEST_SKIPPED} -ne 1 ]] || [[ "${ITEST_KEEP_SCRATCH:-0}" == "1" ]]; then
      itest_log "keeping scratch dir for inspection: ${ITEST_SCRATCH}"
    else
      rm -rf "${ITEST_SCRATCH}"
    fi
  fi
  if [[ ${ITEST_SKIPPED} -eq 1 ]]; then
    exit 0
  fi
  if [[ ${rc} -ne 0 ]]; then
    itest_log "exiting nonzero (rc=${rc})"
    exit "${rc}"
  fi
  if [[ ${ITEST_FAIL_COUNT} -gt 0 ]]; then
    exit 1
  fi
  exit 0
}

# Prints a final PASS/FAIL summary line; caller still relies on itest_cleanup
# (via the EXIT trap) to translate ITEST_FAIL_COUNT into the process exit code.
itest_report() {
  itest_log "=== summary: ${ITEST_PASS_COUNT} passed, ${ITEST_FAIL_COUNT} failed ==="
}

itest_track_pid() {
  ITEST_PIDS+=("$1")
  # Also persist to a file: itest_start_subagent/itest_start_snmpd are often
  # invoked as `x="$(itest_start_foo ...)"`, which runs the function (and
  # this call) in a *subshell* -- appends to the ITEST_PIDS array there are
  # invisible to the parent shell's copy. The file survives that boundary,
  # so itest_cleanup (which always runs in the top-level scenario shell,
  # never in a subshell) can still find and kill every tracked process.
  if [[ -n "${ITEST_SCRATCH:-}" ]]; then
    echo "$1" >> "${ITEST_SCRATCH}/.tracked_pids" 2>/dev/null || true
  fi
}

# --- port allocation (design says 16100-16300) ----------------------------

# Returns a free TCP/UDP port in [16100,16300) via a short python3 probe;
# falls back to a pseudo-random pick in-range if python3 is unavailable.
itest_free_port() {
  local base=16100 span=200
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$base" "$span" <<'PYEOF'
import socket, sys, random
base, span = int(sys.argv[1]), int(sys.argv[2])
for _ in range(200):
    p = base + random.randrange(span)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", p))
        print(p)
        break
    except OSError:
        continue
    finally:
        s.close()
PYEOF
  else
    echo $((base + RANDOM % span))
  fi
}

# --- binary / capability gates ---------------------------------------------

# Missing build output is a FAILURE, not a skip.
#
# These helpers used to skip when a binary was absent, as scaffolding for the
# period when src/main.c was still being written. That reason is gone, and the
# behaviour is actively dangerous now: pointing the suite at a build directory
# that does not exist made every scenario skip and the whole run exit 0, which
# reads as "everything passed" in CI. A skip is for a capability the
# environment cannot provide (see scenario g and dm-flakey); not having built
# the thing under test is not that.
#
# ITEST_ALLOW_MISSING_BUILD=1 restores the old skipping behaviour for anyone
# who deliberately wants to run a subset against a partial build.
itest_missing_build() {
  local what="$1"
  if [[ "${ITEST_ALLOW_MISSING_BUILD:-0}" == "1" ]]; then
    itest_skip "${what} (ITEST_ALLOW_MISSING_BUILD=1)"
  fi
  itest_fail "${what}"
  exit 1
}

# Verifies the subagent binary exists and actually runs (-h exits 0).
itest_require_subagent() {
  if [[ ! -x "${ITEST_SUBAGENT_BIN}" ]]; then
    itest_missing_build "agentx-subagent binary not found at ${ITEST_SUBAGENT_BIN} -- build it first (see README), or set ITEST_BUILD_DIR to the right directory"
  fi
  if ! "${ITEST_SUBAGENT_BIN}" -h >/dev/null 2>&1; then
    itest_missing_build "agentx-subagent -h did not exit 0; binary is broken/incomplete"
  fi
}

itest_require_storage_cli() {
  if [[ ! -x "${ITEST_STORAGE_CLI}" ]]; then
    itest_missing_build "storage_cli helper not found at ${ITEST_STORAGE_CLI}"
  fi
}

itest_require_rust_bins() {
  if [[ ! -x "${ITEST_TELEMETRY_BIN}" || ! -x "${ITEST_LOADGEN_BIN}" ]]; then
    itest_missing_build "rust-app release binaries not found under ${ITEST_RUST_RELEASE_DIR} (run: cd rust-app && cargo build --release)"
  fi
}

itest_require_it_helper() {
  if [[ ! -x "${ITEST_IT_HELPER_BIN}" ]]; then
    itest_missing_build "it_helper binary not found at ${ITEST_IT_HELPER_BIN} (run: cd tests/rust_helpers && cargo build --release)"
  fi
}

# --- snmpd / subagent lifecycle ---------------------------------------------

# itest_start_snmpd <scratch_dir> <agentx_sock_path> <snmp_udp_port> <trap_udp_port>
# Renders tests/snmpd.conf into the scratch dir and starts snmpd in the
# foreground as a background job; blocks (with a timeout) until it responds.
itest_start_snmpd() {
  local scratch="$1" agentx_sock="$2" snmp_port="$3" trap_port="$4"
  local conf="${scratch}/snmpd.conf"
  sed -e "s#@@AGENTX_SOCK@@#unix:${agentx_sock}#" \
      -e "s#@@TRAP_PORT@@#udp:127.0.0.1:${trap_port}#" \
      "${ITEST_TESTS_DIR}/snmpd.conf" > "${conf}"

  /usr/sbin/snmpd -f -Lo -C -c "${conf}" \
    -p "${scratch}/snmpd.pid" \
    "udp:127.0.0.1:${snmp_port}" \
    > "${scratch}/snmpd.log" 2>&1 &
  local pid=$!
  itest_track_pid "${pid}"

  local waited=0
  while [[ ${waited} -lt 100 ]]; do
    if snmpget -v2c -c public -t 1 -r 0 "127.0.0.1:${snmp_port}" .1.3.6.1.2.1.1.1.0 \
         >/dev/null 2>&1; then
      echo "${pid}"
      return 0
    fi
    if ! kill -0 "${pid}" 2>/dev/null; then
      itest_log "snmpd exited early; log follows:"
      cat "${scratch}/snmpd.log" >&2 || true
      return 1
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  itest_log "timed out waiting for snmpd to answer on port ${snmp_port}"
  cat "${scratch}/snmpd.log" >&2 || true
  return 1
}

# itest_start_subagent <config_dir> <cache_dir> <ipc_sock> <agentx_sock> <log_file>
# Starts the subagent in the foreground as a background job; blocks (with a
# timeout) until the AgentX unix socket and the IPC socket both exist AND the
# subagent has logged a successful AgentX connect.
itest_start_subagent() {
  local config_dir="$1" cache_dir="$2" ipc_sock="$3" agentx_sock="$4" log_file="$5"
  mkdir -p "${config_dir}" "${cache_dir}"

  "${ITEST_SUBAGENT_BIN}" -f -L o \
    -x "unix:${agentx_sock}" \
    -C "${config_dir}" \
    -c "${cache_dir}" \
    -s "${ipc_sock}" \
    > "${log_file}" 2>&1 &
  local pid=$!
  itest_track_pid "${pid}"

  local waited=0
  while [[ ${waited} -lt 100 ]]; do
    if [[ -S "${ipc_sock}" ]] && grep -qi "connected" "${log_file}" 2>/dev/null; then
      echo "${pid}"
      return 0
    fi
    if ! kill -0 "${pid}" 2>/dev/null; then
      itest_log "agentx-subagent exited early; log follows:"
      cat "${log_file}" >&2 || true
      return 1
    fi
    sleep 0.1
    waited=$((waited + 1))
  done
  itest_log "timed out waiting for agentx-subagent to come up; log follows:"
  cat "${log_file}" >&2 || true
  return 1
}

# Waits (bounded) for a path to appear (socket, mount, file).
itest_wait_for_path() {
  local path="$1" timeout_iters="${2:-50}"
  local waited=0
  while [[ ! -e "${path}" && ${waited} -lt ${timeout_iters} ]]; do
    sleep 0.1
    waited=$((waited + 1))
  done
  [[ -e "${path}" ]]
}

# --- numeric helpers ---------------------------------------------------------

# itest_percentile <p 0-100> <file with one number per line, sorted ascending>
itest_percentile() {
  local p="$1" file="$2"
  local n
  n=$(wc -l < "${file}")
  [[ "${n}" -gt 0 ]] || { echo 0; return; }
  local idx=$(( (n - 1) * p / 100 + 1 ))
  [[ ${idx} -lt 1 ]] && idx=1
  [[ ${idx} -gt ${n} ]] && idx=${n}
  sed -n "${idx}p" "${file}"
}
