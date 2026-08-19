#!/usr/bin/env bash
# scenario_h_table.sh -- conceptual tables over AgentX (docs/design.md 3.2).
#
# Everything in this suite up to here exercises scalars. A table adds three
# things that can only be checked against a real master agent:
#
#   1. rows have to be discovered from the LMDB key space on every request,
#      and reported in OID order -- which is NOT the order the keys sort in
#      (bytewise, "sensorName.10" precedes "sensorName.2");
#   2. a row's cells may be split across config.lmdb and cache.lmdb, because
#      storage_mode.h selects the backend per column, and both a walk and a
#      Set have to work across that seam;
#   3. rows come and go underneath the agent: the Rust app writes and deletes
#      telemetry rows while a walk may be in flight, and a row may exist with
#      some of its cells missing.
#
# Checks here, in order:
#   a) the provisioned portConfigTable rows are walkable and carry their MIB
#      DEFVALs (include/table_provision.h)
#   b) each column's cell lands in the environment storage_mode.h names, and
#      an SNMP Set of a table cell is visible to a separate process
#   c) a Set of two columns of one row in a single PDU works and does not
#      take the subagent down -- net-snmp hands the same row context to both
#      varbinds, which a per-row allocation would double free
#   d) a rejected varbind leaves the other cells of the PDU untouched
#   e) rows cannot be created or destroyed by a manager (no RowStatus)
#   f) sensorTable is empty until the Rust telemetry app writes rows, then
#      reports exactly what it wrote, and empties again when it deletes them
#   g) a row with a missing cell reports noSuchInstance for that cell and
#      does not interrupt the walk
#   h) cell keys whose instance is not canonical are ignored, not reported
#   i) the per-column split survives a restart: the persistent columns keep
#      their Set values, the volatile column returns to its DEFVAL
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_h"
trap itest_cleanup EXIT

itest_require_subagent
itest_require_storage_cli
itest_require_rust_bins

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"
TARGET="127.0.0.1:${SNMP_PORT}"

# AGENTX-DEMO-MIB, numerically so the scenario does not depend on the MIB
# file being installed where the snmp tools look.
PORT_TABLE=".1.3.6.1.4.1.99999.1.5"
COL_PORT_DESCR="${PORT_TABLE}.1.2"
COL_PORT_ADMIN="${PORT_TABLE}.1.3"
COL_PORT_ALARM="${PORT_TABLE}.1.4"
SENSOR_TABLE=".1.3.6.1.4.1.99999.2.5"
COL_SENSOR_NAME="${SENSOR_TABLE}.1.2"
COL_SENSOR_TEMP="${SENSOR_TABLE}.1.3"
COL_SENSOR_COUNT="${SENSOR_TABLE}.1.4"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent_1.log")"

snmp_get() { snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" "$1" 2>&1; }
snmp_walk() { snmpwalk -v2c -c public -On -Oq -t 2 -r 1 "${TARGET}" "$1" 2>&1; }

# --- a) provisioned rows -----------------------------------------------
walk="$(snmp_walk "${PORT_TABLE}")"
rows="$(echo "${walk}" | grep -c "^${COL_PORT_DESCR}\.")"
if [[ "${rows}" -eq 4 ]]; then
  itest_pass "portConfigTable walks the 4 provisioned rows (TABLE_ROWS_portConfigTable)"
else
  itest_fail "expected 4 provisioned portConfigTable rows, walk reported ${rows}: ${walk}"
fi

cells="$(echo "${walk}" | grep -c "^${PORT_TABLE}\.")"
if [[ "${cells}" -eq 12 ]]; then
  itest_pass "portConfigTable walk returns all 4 rows x 3 accessible columns"
else
  itest_fail "expected 12 portConfigTable cells, walk returned ${cells}: ${walk}"
fi

descr_defval="$(snmp_get "${COL_PORT_DESCR}.3")"
alarm_defval="$(snmp_get "${COL_PORT_ALARM}.3")"
if [[ "${descr_defval}" == '"port"' && "${alarm_defval}" == "80000" ]]; then
  itest_pass "provisioned rows carry the MIB DEFVALs (portDescr='port', portAlarmThresholdMilliC=80000)"
else
  itest_fail "unexpected DEFVALs on row 3: portDescr=${descr_defval} portAlarmThresholdMilliC=${alarm_defval}"
fi

# The index column is not-accessible, so it must not appear in the walk even
# though a manager can address rows by it.
if echo "${walk}" | grep -q "^${PORT_TABLE}\.1\.1\."; then
  itest_fail "not-accessible index column portIndex was returned by the walk"
else
  itest_pass "not-accessible index column portIndex is absent from the walk"
fi

# --- b) per-column storage split ---------------------------------------
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${COL_PORT_DESCR}.2" s "uplink-a" >/dev/null
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${COL_PORT_ALARM}.2" i 91000 >/dev/null
sleep 0.2

config_descr="$("${ITEST_STORAGE_CLI}" get "${CONFIG_DIR}" rdonly bytes portDescr.2 2>/dev/null || echo "<missing>")"
cache_alarm="$("${ITEST_STORAGE_CLI}" get "${CACHE_DIR}" nosync-rdonly int portAlarmThresholdMilliC.2 2>/dev/null || echo "<missing>")"
if [[ "${config_descr}" == "uplink-a" ]]; then
  itest_pass "portDescr.2 (STORAGE_MODE_PERSISTENT) was written to config.lmdb"
else
  itest_fail "portDescr.2 not found in config.lmdb: '${config_descr}'"
fi
if [[ "${cache_alarm}" == "91000" ]]; then
  itest_pass "portAlarmThresholdMilliC.2 (STORAGE_MODE_VOLATILE) was written to cache.lmdb"
else
  itest_fail "portAlarmThresholdMilliC.2 not found in cache.lmdb: '${cache_alarm}'"
fi
if "${ITEST_STORAGE_CLI}" get "${CONFIG_DIR}" rdonly int portAlarmThresholdMilliC.2 >/dev/null 2>&1; then
  itest_fail "portAlarmThresholdMilliC.2 also exists in config.lmdb -- the per-column split is not being honoured"
else
  itest_pass "portAlarmThresholdMilliC.2 exists only in cache.lmdb, not in both"
fi

# One row, cells in two environments, read back over SNMP.
row2_descr="$(snmp_get "${COL_PORT_DESCR}.2")"
row2_alarm="$(snmp_get "${COL_PORT_ALARM}.2")"
if [[ "${row2_descr}" == '"uplink-a"' && "${row2_alarm}" == "91000" ]]; then
  itest_pass "a single row straddling config.lmdb and cache.lmdb reads back whole over SNMP"
else
  itest_fail "row 2 did not read back correctly across the two environments: portDescr=${row2_descr} portAlarmThresholdMilliC=${row2_alarm}"
fi

# --- c) two columns of one row in one PDU ------------------------------
if snmpset -v2c -c public -t 2 -r 1 "${TARGET}" \
     "${COL_PORT_DESCR}.1" s "both-at-once" \
     "${COL_PORT_ADMIN}.1" i 2 >/dev/null 2>&1; then
  itest_pass "a PDU setting two columns of the same row succeeds"
else
  itest_fail "a PDU setting two columns of the same row was rejected"
fi
sleep 0.2
if kill -0 "${subagent_pid}" 2>/dev/null; then
  itest_pass "subagent still running after a multi-column Set on one row (no double free of the row context)"
else
  itest_fail "subagent died on a multi-column Set of one row"
  cat "${ITEST_SCRATCH}/subagent_1.log" >&2 || true
fi
if [[ "$(snmp_get "${COL_PORT_DESCR}.1")" == '"both-at-once"' && "$(snmp_get "${COL_PORT_ADMIN}.1")" == "2" ]]; then
  itest_pass "both cells of the multi-column Set took effect"
else
  itest_fail "multi-column Set did not apply to both cells"
fi

# --- d) a rejected varbind leaves its neighbours alone -----------------
before_descr="$(snmp_get "${COL_PORT_DESCR}.4")"
set_out="$(snmpset -v2c -c public -t 2 -r 1 "${TARGET}" \
             "${COL_PORT_DESCR}.4" s "must-not-stick" \
             "${COL_PORT_ADMIN}.4" i 99 2>&1 || true)"
if echo "${set_out}" | grep -qi "wrongValue"; then
  itest_pass "an out-of-enum value on portAdminStatus is rejected with wrongValue"
else
  itest_fail "expected wrongValue for portAdminStatus=99, got: ${set_out}"
fi
if [[ "$(snmp_get "${COL_PORT_DESCR}.4")" == "${before_descr}" ]]; then
  itest_pass "the accompanying portDescr varbind was not applied (Set stays all-or-nothing)"
else
  itest_fail "portDescr.4 changed even though the PDU failed: ${before_descr} -> $(snmp_get "${COL_PORT_DESCR}.4")"
fi

# --- e) no row creation or destruction by a manager --------------------
create_out="$(snmpset -v2c -c public -t 2 -r 1 "${TARGET}" "${COL_PORT_DESCR}.9" s "ghost" 2>&1 || true)"
if echo "${create_out}" | grep -qi "noCreation"; then
  itest_pass "setting a cell of a non-existent row is refused with noCreation (no RowStatus by design)"
else
  itest_fail "expected noCreation for a non-existent row, got: ${create_out}"
fi
if "${ITEST_STORAGE_CLI}" get "${CONFIG_DIR}" rdonly bytes portDescr.9 >/dev/null 2>&1; then
  itest_fail "the refused Set still created portDescr.9 in config.lmdb"
else
  itest_pass "the refused Set wrote nothing to storage"
fi

# --- f) telemetry rows appear, then disappear --------------------------
before_rows="$(snmp_walk "${SENSOR_TABLE}")"
if echo "${before_rows}" | grep -q "No Such Object"; then
  itest_pass "sensorTable is empty before the telemetry app runs (read-only rows are never seeded)"
else
  itest_fail "sensorTable was not empty on a fresh cache: ${before_rows}"
fi

"${ITEST_TELEMETRY_BIN}" --cache-dir "${CACHE_DIR}" --config-dir "${CONFIG_DIR}" --once >/dev/null
sleep 0.2

sensor_rows="$(snmp_walk "${COL_SENSOR_NAME}" | grep -c "^${COL_SENSOR_NAME}\.")"
if [[ "${sensor_rows}" -eq 3 ]]; then
  itest_pass "sensorTable reports the 3 rows the Rust telemetry app wrote"
else
  itest_fail "expected 3 sensorTable rows after one telemetry sample, got ${sensor_rows}"
fi

name1="$(snmp_get "${COL_SENSOR_NAME}.1")"
count1="$(snmp_get "${COL_SENSOR_COUNT}.1")"
store_name1="$("${ITEST_STORAGE_CLI}" get "${CACHE_DIR}" nosync-rdonly bytes sensorName.1 2>/dev/null || echo "<missing>")"
if [[ "${name1}" == "\"${store_name1}\"" && -n "${store_name1}" ]]; then
  itest_pass "sensorName.1 read over AgentX matches the cell in cache.lmdb ('${store_name1}')"
else
  itest_fail "sensorName.1 mismatch: SNMP=${name1} storage=${store_name1}"
fi
if [[ "${count1}" == "1" ]]; then
  itest_pass "sensorSampleCount.1 decodes as Counter32=1 (the writer's Uint32 and the handler agree)"
else
  itest_fail "sensorSampleCount.1 was '${count1}', expected 1 -- a type mismatch between writer and handler reads as genErr"
fi

# --- g) a row with a hole ----------------------------------------------
"${ITEST_STORAGE_CLI}" set "${CACHE_DIR}" nosync bytes sensorName.9 "partial-row" >/dev/null
sleep 0.2

hole="$(snmp_get "${COL_SENSOR_TEMP}.9")"
if echo "${hole}" | grep -qi "No Such Instance"; then
  itest_pass "a missing cell of an existing row reports noSuchInstance rather than a made-up value"
else
  itest_fail "expected noSuchInstance for the missing sensorTempMilliC.9, got: ${hole}"
fi

walk_after_hole="$(snmp_walk "${SENSOR_TABLE}")"
if echo "${walk_after_hole}" | grep -q "^${COL_SENSOR_NAME}\.9 " &&
   echo "${walk_after_hole}" | grep -q "^${COL_SENSOR_COUNT}\.3 "; then
  itest_pass "the walk reports the partial row and still reaches the columns past the hole"
else
  itest_fail "the walk stalled around the sparse row: ${walk_after_hole}"
fi

# --- rows come back in OID order, not key order ------------------------
"${ITEST_STORAGE_CLI}" set "${CACHE_DIR}" nosync bytes sensorName.10 "tenth" >/dev/null
sleep 0.2
order="$(snmp_walk "${COL_SENSOR_NAME}" | sed "s#^${COL_SENSOR_NAME}\.##" | cut -d' ' -f1 | tr '\n' ' ')"
if [[ "${order}" == "1 2 3 9 10 " ]]; then
  itest_pass "rows are reported in OID order (1 2 3 9 10), not in the bytewise key order LMDB stores them in"
else
  itest_fail "unexpected row order from the walk: '${order}'"
fi

# --- h) keys that are not row instances --------------------------------
"${ITEST_STORAGE_CLI}" set "${CACHE_DIR}" nosync bytes sensorName.007 "leading-zero" >/dev/null
"${ITEST_STORAGE_CLI}" set "${CACHE_DIR}" nosync bytes sensorName.abc "not-a-number" >/dev/null
sleep 0.2
order_after_junk="$(snmp_walk "${COL_SENSOR_NAME}" | sed "s#^${COL_SENSOR_NAME}\.##" | cut -d' ' -f1 | tr '\n' ' ')"
if [[ "${order_after_junk}" == "${order}" ]]; then
  itest_pass "cell keys whose instance is not canonical dotted decimal are ignored, not reported as rows"
else
  itest_fail "malformed cell keys changed the walk: '${order_after_junk}'"
fi
if grep -q "not canonical dotted decimal" "${ITEST_SCRATCH}/subagent_1.log"; then
  itest_pass "the subagent logged the ignored keys instead of failing silently"
else
  itest_fail "no log line about the ignored malformed keys"
fi

# --- telemetry rows disappear when their cells are deleted -------------
"${ITEST_TELEMETRY_BIN}" --cache-dir "${CACHE_DIR}" --config-dir "${CONFIG_DIR}" --clear-sensor-rows >/dev/null
sleep 0.2
after_clear="$(snmp_walk "${COL_SENSOR_NAME}" | sed "s#^${COL_SENSOR_NAME}\.##" | cut -d' ' -f1 | tr '\n' ' ')"
if [[ "${after_clear}" == "9 10 " ]]; then
  itest_pass "deleting a row's cells removes it from the table (only the hand-written rows remain)"
else
  itest_fail "expected only rows 9 and 10 after clearing the telemetry rows, got: '${after_clear}'"
fi

# --- i) the per-column split across a restart --------------------------
kill "${subagent_pid}" 2>/dev/null || true
wait "${subagent_pid}" 2>/dev/null || true
sleep 0.3

# Wipe the cache exactly as a tmpfs remount would (docs/design.md 2.2).
rm -rf "${CACHE_DIR}"
mkdir -p "${CACHE_DIR}"

subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent_2.log")"

after_descr="$(snmp_get "${COL_PORT_DESCR}.2")"
after_alarm="$(snmp_get "${COL_PORT_ALARM}.2")"
if [[ "${after_descr}" == '"uplink-a"' ]]; then
  itest_pass "the persistent column kept its Set value across a restart with a wiped cache"
else
  itest_fail "portDescr.2 after restart was ${after_descr}, expected \"uplink-a\""
fi
if [[ "${after_alarm}" == "80000" ]]; then
  itest_pass "the volatile column of the same row came back at its DEFVAL (80000), not at the value that was Set"
else
  itest_fail "portAlarmThresholdMilliC.2 after restart was ${after_alarm}, expected the DEFVAL 80000"
fi

after_sensors="$(snmp_walk "${SENSOR_TABLE}")"
if echo "${after_sensors}" | grep -q "No Such Object"; then
  itest_pass "sensorTable is empty again after the cache wipe: telemetry rows are never seeded"
else
  itest_fail "sensorTable still reported rows after the cache was wiped: ${after_sensors}"
fi

itest_report
