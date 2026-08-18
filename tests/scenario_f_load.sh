#!/usr/bin/env bash
# scenario_f_load.sh -- docs/design.md 5.3 / 5.1 item 5.
#
# High-frequency SNMP GET load: a large snmpwalk/snmpget flood, reporting
# throughput and latency percentiles -- the empirical evidence for the LMDB
# choice in design.md 2.1 (mmap reads, no SQL parse/B-tree traversal
# overhead, readers never block on writers).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_f"
trap itest_cleanup EXIT

itest_require_subagent

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"
TARGET="127.0.0.1:${SNMP_PORT}"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent.log" >/dev/null

OID_ROOT=".1.3.6.1.4.1.99999"
OID_CPU_TEMP=".1.3.6.1.4.1.99999.2.1.0"

# --- flood of individual GETs, single-key hot path -------------------------
GET_COUNT=500
GET_LOG="${ITEST_SCRATCH}/get_latencies_us.txt"
: > "${GET_LOG}"
get_errors=0
t_start=$(date +%s%N)
for ((i = 0; i < GET_COUNT; i++)); do
  t0=$(date +%s%N)
  if ! snmpget -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_CPU_TEMP}" >/dev/null 2>>"${ITEST_SCRATCH}/get_errors.log"; then
    get_errors=$((get_errors + 1))
  fi
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000 )) >> "${GET_LOG}"
done
t_end=$(date +%s%N)

sort -n "${GET_LOG}" -o "${GET_LOG}"
total_s=$(awk -v a="${t_start}" -v b="${t_end}" 'BEGIN{printf "%.6f", (b-a)/1e9}')
throughput=$(awk -v n="${GET_COUNT}" -v s="${total_s}" 'BEGIN{if (s>0) printf "%.1f", n/s; else print "inf"}')
p50=$(itest_percentile 50 "${GET_LOG}")
p95=$(itest_percentile 95 "${GET_LOG}")
p99=$(itest_percentile 99 "${GET_LOG}")

itest_log "single-OID GET flood: n=${GET_COUNT} errors=${get_errors} wall=${total_s}s throughput=${throughput} ops/s p50=${p50}us p95=${p95}us p99=${p99}us"
if [[ ${get_errors} -eq 0 ]]; then
  itest_pass "GET flood (${GET_COUNT} requests) completed with zero errors: throughput=${throughput} ops/s p50=${p50}us p95=${p95}us p99=${p99}us"
else
  itest_fail "${get_errors}/${GET_COUNT} GETs failed during the flood"
fi

# --- repeated full-subtree snmpwalk, large-scan pattern --------------------
WALK_ITERATIONS=50
WALK_LOG="${ITEST_SCRATCH}/walk_latencies_ms.txt"
: > "${WALK_LOG}"
walk_errors=0
for ((i = 0; i < WALK_ITERATIONS; i++)); do
  t0=$(date +%s%N)
  if ! snmpwalk -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_ROOT}" >/dev/null 2>>"${ITEST_SCRATCH}/walk_errors.log"; then
    walk_errors=$((walk_errors + 1))
  fi
  t1=$(date +%s%N)
  echo $(( (t1 - t0) / 1000000 )) >> "${WALK_LOG}"
done
sort -n "${WALK_LOG}" -o "${WALK_LOG}"
wp50=$(itest_percentile 50 "${WALK_LOG}")
wp95=$(itest_percentile 95 "${WALK_LOG}")
wp99=$(itest_percentile 99 "${WALK_LOG}")

itest_log "full-tree snmpwalk flood: n=${WALK_ITERATIONS} errors=${walk_errors} p50=${wp50}ms p95=${wp95}ms p99=${wp99}ms"
if [[ ${walk_errors} -eq 0 ]]; then
  itest_pass "snmpwalk flood (${WALK_ITERATIONS} full-tree walks) completed with zero errors: p50=${wp50}ms p95=${wp95}ms p99=${wp99}ms"
else
  itest_fail "${walk_errors}/${WALK_ITERATIONS} snmpwalks failed during the flood"
fi

itest_report
