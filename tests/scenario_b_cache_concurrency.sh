#!/usr/bin/env bash
# scenario_b_cache_concurrency.sh -- docs/design.md 5.3, item 2.
#
# cache.lmdb concurrency: the Rust telemetry app (its sole writer, per
# design.md 2.5) writes at high frequency while several reader processes
# (C storage_cli and the Rust it_helper) read concurrently. Asserts:
#   - readers never block on the writer (bounded wall-clock time)
#   - readers never observe a torn/partial value (decode never fails)
#   - the writer completes all its scheduled samples (not starved by readers)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_b"
trap itest_cleanup EXIT

itest_require_subagent
itest_require_storage_cli
itest_require_rust_bins
itest_require_it_helper

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent.log" >/dev/null
# Subagent seeds cache.lmdb defaults on open (design.md 2.3); telemetry then
# becomes the sole writer of the telemetry keys per design.md 2.5.

WRITER_ITERATIONS=200
READERS=4
READER_ITERATIONS=200

# --- start the Rust writer --------------------------------------------------
writer_log="${ITEST_SCRATCH}/telemetry.log"
"${ITEST_TELEMETRY_BIN}" \
  --cache-dir "${CACHE_DIR}" \
  --config-dir "${CONFIG_DIR}" \
  --iterations "${WRITER_ITERATIONS}" \
  --interval-sec 0 \
  --seed 7 \
  > "${writer_log}" 2>&1 &
writer_pid=$!
itest_track_pid "${writer_pid}"

writer_start_ns=$(date +%s%N)

# --- start concurrent readers (mix of C and Rust) while the writer runs ----
reader_pids=()
for ((r = 0; r < READERS; r++)); do
  if (( r % 2 == 0 )); then
    "${ITEST_STORAGE_CLI}" watch "${CACHE_DIR}" nosync cpuTempMilliC "${READER_ITERATIONS}" 500 \
      > "${ITEST_SCRATCH}/reader_c_${r}.log" 2>&1 &
  else
    "${ITEST_IT_HELPER_BIN}" \
      cache-watch "${CACHE_DIR}" sampleCount "${READER_ITERATIONS}" 0 \
      > "${ITEST_SCRATCH}/reader_rust_${r}.log" 2>&1 &
  fi
  reader_pids+=("$!")
done

for pid in "${reader_pids[@]}"; do
  wait "${pid}" || true
done
wait "${writer_pid}"
writer_end_ns=$(date +%s%N)
writer_wall_s=$(awk -v a="${writer_start_ns}" -v b="${writer_end_ns}" 'BEGIN{printf "%.3f", (b-a)/1e9}')

# --- assertions -------------------------------------------------------------

writer_samples=$(grep -c '^\[telemetry\] sample=' "${writer_log}" || true)
if [[ "${writer_samples}" -eq "${WRITER_ITERATIONS}" ]]; then
  itest_pass "writer completed all ${WRITER_ITERATIONS} samples in ${writer_wall_s}s despite ${READERS} concurrent readers (not starved/blocked)"
else
  itest_fail "writer only completed ${writer_samples}/${WRITER_ITERATIONS} samples"
fi

any_reader_error=0
for ((r = 0; r < READERS; r++)); do
  logf="${ITEST_SCRATCH}/reader_c_${r}.log"
  [[ -f "${logf}" ]] || logf="${ITEST_SCRATCH}/reader_rust_${r}.log"
  [[ -f "${logf}" ]] || continue
  errs=$(grep -oE 'errors=[0-9]+' "${logf}" | head -1 | cut -d= -f2 || echo 0)
  errs="${errs:-0}"
  if [[ "${errs}" != "0" ]]; then
    any_reader_error=1
    itest_log "reader ${r} reported ${errs} decode errors: $(cat "${logf}")"
  fi
done
if [[ ${any_reader_error} -eq 0 ]]; then
  itest_pass "all ${READERS} concurrent readers (C storage_cli + Rust it_helper) saw zero torn/partial reads"
else
  itest_fail "at least one reader observed a torn/partial (undecodable) value"
fi

itest_report
