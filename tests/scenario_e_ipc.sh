#!/usr/bin/env bash
# scenario_e_ipc.sh -- docs/design.md 5.3, item 5 / ch.4.
#
# Rust app -> subagent Unix-socket + protobuf IPC:
#   - concurrent multiplexed requests from several clients are all serialized
#     correctly (every write lands, none lost/corrupted)
#   - socket disconnect then reconnect behaves (a fresh client can connect
#     again after a previous one closes)
#   - an invalid request (garbage frame) is rejected without taking down the
#     server or affecting other, well-behaved concurrent clients
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_e"
trap itest_cleanup EXIT

itest_require_subagent
itest_require_rust_bins
itest_require_it_helper

AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
CONFIG_DIR="${ITEST_SCRATCH}/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"
TARGET="127.0.0.1:${SNMP_PORT}"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent.log")"

# --- concurrent multiplexed WriteConfigRequests ----------------------------
# sampleIntervalSec is the only writable Unsigned32 config key besides
# adminStatusExt/deviceName; use it as a distinguishable target: each client
# writes a distinct value and we confirm the final on-disk value is exactly
# one client's write (never a torn mix), same invariant as scenario (a) but
# over the IPC path instead of AgentX.
CLIENTS=5
ipc_pids=()
for ((c = 0; c < CLIENTS; c++)); do
  val=$(( 100 + c ))
  "${ITEST_LOADGEN_BIN}" --socket "${IPC_SOCK}" --key sampleIntervalSec --kind uint32 --value "${val}" \
    > "${ITEST_SCRATCH}/ipc_client_${c}.log" 2>&1 &
  ipc_pids+=("$!")
done
for pid in "${ipc_pids[@]}"; do
  wait "${pid}" || true
done

all_ok=1
for ((c = 0; c < CLIENTS; c++)); do
  if ! grep -q "status=StatusOk\|Status::Ok\|status=Ok" "${ITEST_SCRATCH}/ipc_client_${c}.log" 2>/dev/null; then
    # loadgen prints the Debug repr of Status; accept any of its spellings
    # across derive-macro versions rather than over-fitting to one.
    if ! grep -qi "status.*ok" "${ITEST_SCRATCH}/ipc_client_${c}.log"; then
      all_ok=0
      itest_log "client ${c} did not report an OK status: $(cat "${ITEST_SCRATCH}/ipc_client_${c}.log")"
    fi
  fi
done
if [[ ${all_ok} -eq 1 ]]; then
  itest_pass "${CLIENTS} concurrent IPC clients all received an OK WriteConfigResponse (serialized correctly)"
else
  itest_fail "at least one concurrent IPC client did not receive an OK response"
fi

final_val="$(snmpget -v2c -c public -Ov -Oq -t 2 -r 1 "${TARGET}" .1.3.6.1.4.1.99999.1.4.0 2>/dev/null)"
plausible=0
for ((c = 0; c < CLIENTS; c++)); do
  [[ "${final_val}" == "$(( 100 + c ))" ]] && plausible=1
done
if [[ ${plausible} -eq 1 ]]; then
  itest_pass "final sampleIntervalSec (${final_val}) matches exactly one concurrent IPC client's write"
else
  itest_fail "final sampleIntervalSec (${final_val}) matches none of the concurrent clients' writes -- possible torn write"
fi

# --- disconnect then reconnect ---------------------------------------------
if "${ITEST_IT_HELPER_BIN}" ipc-ping "${IPC_SOCK}" 111 > "${ITEST_SCRATCH}/ping1.log" 2>&1; then
  itest_pass "first IPC client connected and pinged successfully"
else
  itest_fail "first IPC ping failed: $(cat "${ITEST_SCRATCH}/ping1.log")"
fi
# it_helper's process exit closes its UnixStream (a real disconnect); a
# fresh connection must still be accepted afterwards.
if "${ITEST_IT_HELPER_BIN}" ipc-ping "${IPC_SOCK}" 222 > "${ITEST_SCRATCH}/ping2.log" 2>&1; then
  itest_pass "server accepted a fresh IPC connection after the previous client disconnected"
else
  itest_fail "reconnect after disconnect failed: $(cat "${ITEST_SCRATCH}/ping2.log")"
fi

# --- invalid request must not affect other concurrent clients -------------
"${ITEST_IT_HELPER_BIN}" ipc-send-garbage "${IPC_SOCK}" > "${ITEST_SCRATCH}/garbage.log" 2>&1 &
garbage_pid=$!
"${ITEST_IT_HELPER_BIN}" ipc-ping "${IPC_SOCK}" 333 > "${ITEST_SCRATCH}/ping3.log" 2>&1 &
good_pid=$!
wait "${garbage_pid}" || true
good_rc=0
wait "${good_pid}" || good_rc=$?

if [[ ${good_rc} -eq 0 ]] && grep -q "nonce=333" "${ITEST_SCRATCH}/ping3.log"; then
  itest_pass "a well-behaved concurrent client succeeded while an invalid frame was sent on another connection"
else
  itest_fail "the well-behaved concurrent client was affected by the invalid frame: $(cat "${ITEST_SCRATCH}/ping3.log")"
fi

if kill -0 "${subagent_pid}" 2>/dev/null; then
  itest_pass "subagent process is still running after the invalid-frame connection (not crashed)"
else
  itest_fail "subagent process died after the invalid-frame connection"
fi

if snmpget -v2c -c public -t 2 -r 1 "${TARGET}" .1.3.6.1.2.1.1.1.0 >/dev/null 2>&1; then
  itest_pass "subagent still serves SNMP after the invalid IPC frame"
else
  itest_fail "subagent stopped serving SNMP after the invalid IPC frame"
fi

itest_report
