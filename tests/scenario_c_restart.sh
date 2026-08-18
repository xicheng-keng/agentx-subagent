#!/usr/bin/env bash
# scenario_c_restart.sh -- docs/design.md 5.3, item 3.
#
# Subagent restart: kill and restart it, assert it reconnects to snmpd, its
# OIDs are served again, config.lmdb data survived, and cache.lmdb volatile
# objects reset to DEFVAL when the cache path is wiped (the tmpfs case) but
# survive when it is not.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_c"
trap itest_cleanup EXIT

itest_require_subagent

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"
TARGET="127.0.0.1:${SNMP_PORT}"

OID_DEVICE_NAME=".1.3.6.1.4.1.99999.1.1.0"        # persistent, config.lmdb
OID_TEMP_THRESHOLD=".1.3.6.1.4.1.99999.1.2.0"     # volatile, cache.lmdb (DEFVAL 70000)

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null

subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent_1.log")"

# Give the persistent object a distinctive value and change the volatile one
# away from its DEFVAL, so a later "did it reset" check is unambiguous.
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_DEVICE_NAME}" s "restart-test-device" >/dev/null
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${OID_TEMP_THRESHOLD}" i 12345 >/dev/null

before_device="$(snmpget -v2c -c public -Ov -Oq "${TARGET}" "${OID_DEVICE_NAME}" 2>/dev/null)"
before_temp="$(snmpget -v2c -c public -Ov -Oq "${TARGET}" "${OID_TEMP_THRESHOLD}" 2>/dev/null)"
itest_log "before restart: deviceName=${before_device} tempThresholdMilliC=${before_temp}"

# --- restart #1: cache.lmdb NOT wiped (persistent-disk case) ---------------
kill "${subagent_pid}" 2>/dev/null || true
wait "${subagent_pid}" 2>/dev/null || true
sleep 0.3

subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent_2.log")"

if grep -qi "connected" "${ITEST_SCRATCH}/subagent_2.log"; then
  itest_pass "subagent reconnected to snmpd's AgentX master socket after restart #1"
else
  itest_fail "subagent did not log an AgentX reconnect after restart #1"
fi

after1_device="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "${OID_DEVICE_NAME}" 2>/dev/null || echo "<error>")"
after1_temp="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "${OID_TEMP_THRESHOLD}" 2>/dev/null || echo "<error>")"
itest_log "after restart #1 (cache not wiped): deviceName=${after1_device} tempThresholdMilliC=${after1_temp}"

if [[ "${after1_device}" == "${before_device}" ]]; then
  itest_pass "config.lmdb (deviceName) survived restart #1: '${after1_device}'"
else
  itest_fail "deviceName changed across restart #1: before='${before_device}' after='${after1_device}'"
fi

if [[ "${after1_temp}" == "${before_temp}" ]]; then
  itest_pass "cache.lmdb (tempThresholdMilliC) survived restart #1 because its path was NOT wiped: '${after1_temp}'"
else
  itest_fail "tempThresholdMilliC unexpectedly changed across a restart with an intact cache path: before='${before_temp}' after='${after1_temp}'"
fi

# --- restart #2: cache.lmdb IS wiped (tmpfs-remount case) ------------------
kill "${subagent_pid}" 2>/dev/null || true
wait "${subagent_pid}" 2>/dev/null || true
sleep 0.3

rm -rf "${CACHE_DIR}"
mkdir -p "${CACHE_DIR}"

subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent_3.log")"

if grep -qi "connected" "${ITEST_SCRATCH}/subagent_3.log"; then
  itest_pass "subagent reconnected to snmpd's AgentX master socket after restart #2 (cache wiped)"
else
  itest_fail "subagent did not log an AgentX reconnect after restart #2"
fi

after2_device="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "${OID_DEVICE_NAME}" 2>/dev/null || echo "<error>")"
after2_temp="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "${OID_TEMP_THRESHOLD}" 2>/dev/null || echo "<error>")"
itest_log "after restart #2 (cache wiped): deviceName=${after2_device} tempThresholdMilliC=${after2_temp}"

if [[ "${after2_device}" == "${before_device}" ]]; then
  itest_pass "config.lmdb (deviceName) still survived restart #2, even though cache.lmdb was wiped: '${after2_device}'"
else
  itest_fail "deviceName was lost/changed across restart #2: before='${before_device}' after='${after2_device}'"
fi

if [[ "${after2_temp}" == "70000" ]]; then
  itest_pass "tempThresholdMilliC reset to its DEFVAL (70000) after cache.lmdb was wiped: '${after2_temp}'"
else
  itest_fail "tempThresholdMilliC did NOT reset to DEFVAL after cache.lmdb was wiped: got '${after2_temp}'"
fi

itest_report
