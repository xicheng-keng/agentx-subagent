#!/usr/bin/env bash
# scenario_d_cross_process.sh -- docs/design.md 5.3, item 4 / 5.1 item 3.
#
# SNMP Set -> storage -> read-back from the other process. This is the
# cross-language interop check that matters most: the C subagent and the
# Rust app each encode/decode the storage_lmdb.h value format independently
# (src/storage_lmdb.c vs rust-app/src/storage/codec.rs). We assert the
# *actual on-disk bytes* agree, not merely that no error occurred:
#
#   1. snmpset a value through the C subagent into config.lmdb.
#   2. Read the raw bytes back two independent ways:
#        a) storage_cli hexdump (C code path, storage_lmdb.h)
#        b) it_helper config-hex (Rust code path, ConfigStore/codec.rs,
#           opened read-only exactly as the design mandates)
#   3. Assert the two hex dumps are byte-for-byte identical.
#   4. Also do it for a volatile (cache.lmdb) object and for a bytes-typed
#      (DisplayString) value, to cover more than one storage_type_t tag.
#   5. Finally, the other direction: the Rust telemetry app writes the
#      read-only status scalars and the C handlers read them back over SNMP.
#      Writer and handler have to agree on the *width* as well as the bytes --
#      a Uint64 cell read by storage_get_uint() is a type mismatch, which the
#      handler can only report as genErr.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_d"
trap itest_cleanup EXIT

itest_require_subagent
itest_require_storage_cli
itest_require_it_helper
itest_require_rust_bins

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

assert_bytes_match() {
  local label="$1" c_hex="$2" rust_hex="$3"
  if [[ -z "${c_hex}" || -z "${rust_hex}" ]]; then
    itest_fail "${label}: one side produced no output (c='${c_hex}' rust='${rust_hex}')"
    return
  fi
  if [[ "${c_hex}" == "${rust_hex}" ]]; then
    itest_pass "${label}: C and Rust agree byte-for-byte (${c_hex})"
  else
    itest_fail "${label}: byte mismatch -- C='${c_hex}' Rust='${rust_hex}'"
  fi
}

# --- case 1: Unsigned32 (STORAGE_TYPE_UINT32) via config.lmdb --------------
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.4.0 u 3600 >/dev/null
sleep 0.2
c_hex="$("${ITEST_STORAGE_CLI}" hexdump "${CONFIG_DIR}" persistent sampleIntervalSec)"
rust_hex="$("${ITEST_IT_HELPER_BIN}" config-hex "${CONFIG_DIR}" sampleIntervalSec)"
assert_bytes_match "sampleIntervalSec (Unsigned32=3600, config.lmdb)" "${c_hex}" "${rust_hex}"

# --- case 2: INTEGER (STORAGE_TYPE_INT32) persistent, negative value -------
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.3.0 i 2 >/dev/null  # down(2)
sleep 0.2
c_hex="$("${ITEST_STORAGE_CLI}" hexdump "${CONFIG_DIR}" persistent adminStatusExt)"
rust_hex="$("${ITEST_IT_HELPER_BIN}" config-hex "${CONFIG_DIR}" adminStatusExt)"
assert_bytes_match "adminStatusExt (INTEGER down=2, config.lmdb)" "${c_hex}" "${rust_hex}"

# --- case 3: DisplayString (STORAGE_TYPE_BYTES) persistent -----------------
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.1.0 s "cross-lang-check" >/dev/null
sleep 0.2
c_hex="$("${ITEST_STORAGE_CLI}" hexdump "${CONFIG_DIR}" persistent deviceName)"
rust_hex="$("${ITEST_IT_HELPER_BIN}" config-hex "${CONFIG_DIR}" deviceName)"
assert_bytes_match "deviceName (DisplayString, config.lmdb)" "${c_hex}" "${rust_hex}"

# --- case 4: volatile INTEGER via cache.lmdb -------------------------------
snmpset -v2c -c public -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.2.0 i -15000 >/dev/null
sleep 0.2
c_hex="$("${ITEST_STORAGE_CLI}" hexdump "${CACHE_DIR}" nosync tempThresholdMilliC)"
rust_hex="$("${ITEST_IT_HELPER_BIN}" cache-hex "${CACHE_DIR}" tempThresholdMilliC)"
assert_bytes_match "tempThresholdMilliC (INTEGER=-15000, cache.lmdb, volatile-but-Set)" "${c_hex}" "${rust_hex}"

# --- confirm the SNMP-visible value round-trips to the same integer too ---
snmp_readback="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.2.0 2>/dev/null)"
if [[ "${snmp_readback}" == "-15000" ]]; then
  itest_pass "SNMP-visible value (-15000) matches what was Set, independent of the raw-byte checks above"
else
  itest_fail "SNMP-visible value '${snmp_readback}' does not match the Set value -15000"
fi

# --- case 5: telemetry-written read-only scalars are readable over SNMP -----
# The writer is the Rust app; the reader is the generated C handler. Every
# cell the app writes has to carry the storage type that handler expects
# (Counter32/Unsigned32 -> STORAGE_TYPE_UINT32), or the GET below answers
# genErr instead of a value.
"${ITEST_TELEMETRY_BIN}" --cache-dir "${CACHE_DIR}" --config-dir "${CONFIG_DIR}" --once >/dev/null
sleep 0.2

sample_count="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.2.2.0 2>&1)"
if [[ "${sample_count}" == "1" ]]; then
  itest_pass "sampleCount (Counter32) reads back as 1 after one telemetry sample"
else
  itest_fail "sampleCount read back as '${sample_count}', expected 1 -- a width mismatch between the Rust writer and storage_get_uint() surfaces as genErr"
fi

last_update="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.2.3.0 2>&1)"
if [[ "${last_update}" =~ ^[0-9]+$ ]] && (( last_update >= 1600000000 )); then
  itest_pass "lastUpdateEpoch (Unsigned32) reads back as a plausible unix time (${last_update})"
else
  itest_fail "lastUpdateEpoch read back as '${last_update}', expected a unix timestamp"
fi

c_hex="$("${ITEST_STORAGE_CLI}" hexdump "${CACHE_DIR}" nosync sampleCount)"
rust_hex="$("${ITEST_IT_HELPER_BIN}" cache-hex "${CACHE_DIR}" sampleCount)"
assert_bytes_match "sampleCount (Counter32 written by the Rust app, cache.lmdb)" "${c_hex}" "${rust_hex}"

itest_report
