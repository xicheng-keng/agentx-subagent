#!/usr/bin/env bash
# scenario_a_config_writer.sh -- docs/design.md 5.3, item 1.
#
# config.lmdb single-writer under load:
#   - hammer concurrent snmpset against the subagent's persistent objects
#   - assert every write is reflected with no loss and no deadlock
#   - assert a concurrent high-rate snmpget/snmpwalk stream keeps responding
#     throughout, and report its latency percentiles
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_a"
trap itest_cleanup EXIT

itest_require_subagent

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent.log" >/dev/null

TARGET="127.0.0.1:${SNMP_PORT}"
OID_SAMPLE_INTERVAL=".1.3.6.1.4.1.99999.1.4.0"   # sampleIntervalSec, Unsigned32 (1..86400)
OID_ADMIN_STATUS=".1.3.6.1.4.1.99999.1.3.0"      # adminStatusExt, INTEGER {up(1),down(2),testing(3)}

WRITERS=8
WRITES_PER_WRITER=25
GET_DURATION_SEC=3

# --- concurrent snmpset hammer over sampleIntervalSec ----------------------
# Each writer writes a distinct final value tag by writer id (base*1000+i),
# clamped into the valid range (1..86400), so the *last* write from each
# writer is identifiable and we can confirm no writer's final value went
# missing from the writer's own perspective (a torn/lost write would show up
# as an snmpset error, which -t/-r below turn into a nonzero exit).
writer_job() {
  local wid="$1"
  local i v
  for ((i = 0; i < WRITES_PER_WRITER; i++)); do
    v=$(( (wid * 1000 + i) % 86400 + 1 ))
    if ! snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_SAMPLE_INTERVAL}" u "${v}" \
         >"${ITEST_SCRATCH}/writer_${wid}.out" 2>&1; then
      echo "FAIL" >> "${ITEST_SCRATCH}/writer_${wid}.status"
      return
    fi
  done
  echo "OK ${v}" >> "${ITEST_SCRATCH}/writer_${wid}.status"
}

wpids=()
for ((w = 0; w < WRITERS; w++)); do
  writer_job "${w}" &
  wpids+=("$!")
done

# --- concurrent high-rate GET stream, running alongside the writers --------
GET_LOG="${ITEST_SCRATCH}/get_latencies.txt"
: > "${GET_LOG}"
get_stream_job() {
  local deadline=$((SECONDS + GET_DURATION_SEC))
  while [[ ${SECONDS} -lt ${deadline} ]]; do
    local t0 t1
    t0=$(date +%s%N)
    if snmpget -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_ADMIN_STATUS}" >/dev/null 2>>"${ITEST_SCRATCH}/get_errors.log"; then
      t1=$(date +%s%N)
      echo $(( (t1 - t0) / 1000 )) >> "${GET_LOG}"
    else
      echo "GET_ERROR" >> "${ITEST_SCRATCH}/get_failures.log"
    fi
  done
}
get_stream_job &
get_pid=$!

for pid in "${wpids[@]}"; do
  wait "${pid}"
done
wait "${get_pid}"

# --- assertions -------------------------------------------------------------

all_writers_ok=1
for ((w = 0; w < WRITERS; w++)); do
  status_file="${ITEST_SCRATCH}/writer_${w}.status"
  if [[ ! -f "${status_file}" ]] || ! grep -q "^OK" "${status_file}"; then
    all_writers_ok=0
    itest_log "writer ${w} status: $(cat "${status_file}" 2>/dev/null || echo MISSING)"
  fi
done
if [[ ${all_writers_ok} -eq 1 ]]; then
  itest_pass "all ${WRITERS} concurrent snmpset writers completed ${WRITES_PER_WRITER} writes each with no error/deadlock"
else
  itest_fail "at least one concurrent snmpset writer failed or deadlocked"
fi

# Final value must be exactly one of the writers' final candidates (last
# writer to commit wins; single-writer serialization means it can never be a
# mix of two writers' bytes).
final_val="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "${OID_SAMPLE_INTERVAL}" 2>/dev/null | awk '{print $NF}')"
plausible=0
for ((w = 0; w < WRITERS; w++)); do
  last=$(grep "^OK" "${ITEST_SCRATCH}/writer_${w}.status" 2>/dev/null | awk '{print $2}')
  [[ "${last}" == "${final_val}" ]] && plausible=1
done
if [[ ${plausible} -eq 1 ]]; then
  itest_pass "final sampleIntervalSec (${final_val}) matches exactly one writer's last write (no torn/mixed value)"
else
  itest_fail "final sampleIntervalSec (${final_val}) does not match any writer's last write"
fi

if [[ -s "${ITEST_SCRATCH}/get_failures.log" ]]; then
  itest_fail "$(wc -l < "${ITEST_SCRATCH}/get_failures.log") snmpget(s) failed while writers were hammering config.lmdb"
else
  itest_pass "concurrent GET stream had zero failures while writers were hammering config.lmdb"
fi

get_count=$(wc -l < "${GET_LOG}" || echo 0)
if [[ ${get_count} -gt 0 ]]; then
  sort -n "${GET_LOG}" -o "${GET_LOG}"
  p50=$(itest_percentile 50 "${GET_LOG}")
  p95=$(itest_percentile 95 "${GET_LOG}")
  p99=$(itest_percentile 99 "${GET_LOG}")
  itest_log "GET latency during write load: n=${get_count} p50=${p50}us p95=${p95}us p99=${p99}us"
  itest_pass "collected ${get_count} GET latency samples during concurrent write load"
else
  itest_fail "no successful GET samples were collected"
fi

itest_report
