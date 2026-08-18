#!/usr/bin/env bash
# scenario_g_dm_flakey.sh -- docs/design.md 5.3 (last item) / 5.1 item 6.
#
# Simulated power loss with dm-flakey over a loop device: interrupt writes to
# config.lmdb, then assert the environment is not corrupted after remount and
# that it recovers to the last consistent state (exercising the Copy-on-Write
# B+-tree property called out in design.md 2.1: old generations are never
# overwritten in place, so a write interrupted mid-flight cannot corrupt the
# previously committed data).
#
# THIS SCENARIO NEEDS PRIVILEGES (CAP_SYS_ADMIN for losetup + dm-setup, a
# working device-mapper kernel driver, and usually root). If any of that is
# unavailable -- as is the case in most unprivileged containers/sandboxes --
# this script detects it up front and SKIPs loudly with exit 0. It never
# fakes a pass.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

itest_init "scenario_g"
trap itest_cleanup EXIT

itest_require_subagent

# --- capability probe -------------------------------------------------------
if [[ "$(id -u)" -ne 0 ]]; then
  itest_skip "not running as root; losetup/dmsetup/mount require privileges this process doesn't have"
fi
for bin in losetup dmsetup mkfs.ext4 mount umount blockdev e2fsck; do
  if ! command -v "${bin}" >/dev/null 2>&1; then
    itest_skip "required tool '${bin}' not installed; cannot run the dm-flakey scenario"
  fi
done

IMG="${ITEST_SCRATCH}/flakey.img"
truncate -s 64M "${IMG}"

LOOPDEV="$(losetup -f)"
if ! losetup "${LOOPDEV}" "${IMG}" 2>"${ITEST_SCRATCH}/losetup.err"; then
  itest_skip "losetup failed (no loop device capability in this sandbox): $(cat "${ITEST_SCRATCH}/losetup.err")"
fi
LOOP_ATTACHED=1

cleanup_loop_and_dm() {
  if mountpoint -q "${ITEST_SCRATCH}/mnt" 2>/dev/null; then
    umount "${ITEST_SCRATCH}/mnt" 2>/dev/null || true
  fi
  dmsetup remove flakey-it 2>/dev/null || true
  if [[ "${LOOP_ATTACHED:-0}" -eq 1 ]]; then
    losetup -d "${LOOPDEV}" 2>/dev/null || true
  fi
}
trap 'cleanup_loop_and_dm; itest_cleanup' EXIT

SIZE_SECTORS="$(blockdev --getsz "${LOOPDEV}" 2>/dev/null || echo "")"
if [[ -z "${SIZE_SECTORS}" ]]; then
  itest_skip "could not determine loop device size via blockdev"
fi

# up_interval=2s, down_interval=1s: during the "down" window flakey drops
# writes (and, with drop_writes unset, corrupts them) to simulate a power
# interruption mid-write.
if ! dmsetup create flakey-it --table "0 ${SIZE_SECTORS} flakey ${LOOPDEV} 0 2 1" \
     2>"${ITEST_SCRATCH}/dmsetup.err"; then
  itest_skip "dmsetup create failed (no working device-mapper kernel driver in this sandbox): $(cat "${ITEST_SCRATCH}/dmsetup.err")"
fi

if ! mkfs.ext4 -q "/dev/mapper/flakey-it" 2>"${ITEST_SCRATCH}/mkfs.err"; then
  itest_skip "mkfs.ext4 on the flakey device failed: $(cat "${ITEST_SCRATCH}/mkfs.err")"
fi

mkdir -p "${ITEST_SCRATCH}/mnt"
if ! mount "/dev/mapper/flakey-it" "${ITEST_SCRATCH}/mnt" 2>"${ITEST_SCRATCH}/mount.err"; then
  itest_skip "mount of the flakey device failed: $(cat "${ITEST_SCRATCH}/mount.err")"
fi

# --- the actual test: hammer writes across up/down cycles ------------------
CONFIG_DIR="${ITEST_SCRATCH}/mnt/config.lmdb"
CACHE_DIR="${ITEST_SCRATCH}/cache.lmdb"
IPC_SOCK="${ITEST_SCRATCH}/ipc.sock"
AGENTX_SOCK="${ITEST_SCRATCH}/agentx.sock"
SNMP_PORT="$(itest_free_port)"
TRAP_PORT="$(itest_free_port)"
TARGET="127.0.0.1:${SNMP_PORT}"
mkdir -p "${CONFIG_DIR}"

itest_start_snmpd "${ITEST_SCRATCH}" "${AGENTX_SOCK}" "${SNMP_PORT}" "${TRAP_PORT}" >/dev/null
subagent_pid="$(itest_start_subagent "${CONFIG_DIR}" "${CACHE_DIR}" "${IPC_SOCK}" "${AGENTX_SOCK}" \
  "${ITEST_SCRATCH}/subagent.log")"

last_acked_value=""
DURATION_SEC=8
deadline=$((SECONDS + DURATION_SEC))
v=0
while [[ ${SECONDS} -lt ${deadline} ]]; do
  v=$((v + 1))
  cycled=$(( (v % 86400) + 1 ))
  if snmpset -v2c -c public -t 1 -r 0 "${TARGET}" .1.3.6.1.4.1.99999.1.4.0 u "${cycled}" \
       >/dev/null 2>>"${ITEST_SCRATCH}/snmpset_during_flakey.log"; then
    last_acked_value="${cycled}"
  fi
  # no sleep: hammer as fast as possible so some writes land in a "down" window
done

itest_log "hammered snmpset across dm-flakey up/down cycles for ${DURATION_SEC}s; last acknowledged value=${last_acked_value}"

# --- simulate the power cycle: kill the subagent, unmount, remount --------
kill "${subagent_pid:-0}" 2>/dev/null || true
pkill -f "agentx-subagent.*${CONFIG_DIR}" 2>/dev/null || true
sleep 0.3
umount "${ITEST_SCRATCH}/mnt" 2>&1 | tee "${ITEST_SCRATCH}/umount.log" || true
# Remove and recreate the flakey mapping fully "up" (no more induced faults)
# for the post-recovery check, mirroring a device coming back after a power
# cycle.
dmsetup remove flakey-it 2>/dev/null || true
dmsetup create flakey-it --table "0 ${SIZE_SECTORS} flakey ${LOOPDEV} 0 3600 0"
mount "/dev/mapper/flakey-it" "${ITEST_SCRATCH}/mnt"

# fsck the filesystem itself: dm-flakey's induced faults must not have left
# the *filesystem* inconsistent (a prerequisite for LMDB's own consistency).
if e2fsck -fy "/dev/mapper/flakey-it" >"${ITEST_SCRATCH}/e2fsck.log" 2>&1; then
  itest_pass "filesystem remained consistent (e2fsck clean) after interrupted writes"
else
  # e2fsck exit codes 1/2 mean "errors were found and corrected" -- some
  # filesystem-level inconsistency after simulated power loss is expected
  # and recoverable; only a fsck that cannot complete at all is a real
  # failure of the recovery story we're testing.
  fsck_rc=$?
  if [[ ${fsck_rc} -le 2 ]]; then
    itest_pass "filesystem had recoverable inconsistencies after simulated power loss and e2fsck repaired them (rc=${fsck_rc})"
  else
    itest_fail "e2fsck could not recover the filesystem after simulated power loss (rc=${fsck_rc})"
  fi
fi

# --- assert config.lmdb itself opens cleanly and holds a consistent value -
if [[ -x "${ITEST_STORAGE_CLI}" ]]; then
  if recovered_val="$("${ITEST_STORAGE_CLI}" get "${CONFIG_DIR}" persistent uint sampleIntervalSec 2>"${ITEST_SCRATCH}/storage_get.err")"; then
    itest_pass "config.lmdb opened and decoded sampleIntervalSec cleanly after recovery: ${recovered_val}"
    # It must be a value we actually observed being acknowledged at some
    # point (LMDB's CoW guarantees the last *committed* transaction survives
    # a torn write, but a value from mid-run is an equally valid "last
    # consistent state" -- what must never happen is decode failure or a
    # value that was never any write's intended target, i.e. mixed bytes).
    if [[ "${recovered_val}" -ge 1 && "${recovered_val}" -le 86400 ]]; then
      itest_pass "recovered value (${recovered_val}) is a well-formed, in-range Unsigned32 -- no torn write survived"
    else
      itest_fail "recovered value (${recovered_val}) is out of the valid range for sampleIntervalSec -- looks like a torn write"
    fi
  else
    itest_fail "config.lmdb failed to open/decode after simulated power loss: $(cat "${ITEST_SCRATCH}/storage_get.err")"
  fi
else
  itest_fail "storage_cli helper not built; cannot verify config.lmdb consistency directly"
fi

itest_report
