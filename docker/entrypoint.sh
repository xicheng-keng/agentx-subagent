#!/usr/bin/env bash
# entrypoint.sh -- runtime image entrypoint for agentx-subagent / telemetry.
#
# - creates the directories the two binaries expect (config dir is a real
#   volume; cache dir MUST be tmpfs per docs/design.md 2.3 -- see the
#   Dockerfile/docker-compose.yml comments)
# - waits (bounded) for the AgentX master socket to exist before starting
#   the subagent, since compose/k8s give no ordering guarantee between
#   the snmpd and subagent containers
# - execs the requested binary so it becomes PID 1 and receives signals
#   directly (no orphaned shell holding the TTY/signals hostage)
set -euo pipefail

: "${AGENTX_CONFIG_DB:=/var/lib/agentx-subagent/config.lmdb}"
: "${AGENTX_CACHE_DB:=/run/agentx-subagent/cache.lmdb}"
: "${AGENTX_IPC_SOCKET:=/run/agentx-subagent/ipc.sock}"
: "${AGENTX_MASTER_SOCKET:=unix:/var/agentx/master.sock}"
# Bounded wait for the AgentX master socket, in seconds. 0 disables waiting
# (e.g. for the telemetry binary, which doesn't need snmpd at all).
: "${AGENTX_MASTER_WAIT_SECS:=30}"

log() { printf 'entrypoint: %s\n' "$*" >&2; }

mkdir -p "$(dirname "${AGENTX_CONFIG_DB}")" "$(dirname "${AGENTX_CACHE_DB}")" \
         "$(dirname "${AGENTX_IPC_SOCKET}")"

# Extract the filesystem path out of an AgentX transport spec like
# "unix:/var/agentx/master.sock" for the wait-loop below; leaves non-unix
# transports (tcp:host:port, etc.) alone -- those are reachable without a
# socket file to poll for, so we just proceed without waiting on them.
master_sock_path=""
case "${AGENTX_MASTER_SOCKET}" in
  unix:*) master_sock_path="${AGENTX_MASTER_SOCKET#unix:}" ;;
esac

wait_for_master_socket() {
  [[ -n "${master_sock_path}" ]] || return 0
  [[ "${AGENTX_MASTER_WAIT_SECS}" -gt 0 ]] || return 0

  log "waiting up to ${AGENTX_MASTER_WAIT_SECS}s for AgentX master socket ${master_sock_path} ..."
  local waited=0
  while [[ ! -S "${master_sock_path}" ]]; do
    if [[ ${waited} -ge ${AGENTX_MASTER_WAIT_SECS} ]]; then
      log "timed out waiting for ${master_sock_path}; starting anyway -- the" \
          "subagent retries its own AgentX connect internally, this is just" \
          "an early sanity wait, not the only reconnect mechanism."
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  log "AgentX master socket is present after ${waited}s"
}

cmd="${1:-agentx-subagent}"
shift || true

case "${cmd}" in
  agentx-subagent)
    wait_for_master_socket
    exec /usr/local/bin/agentx-subagent -f -L o \
      -x "${AGENTX_MASTER_SOCKET}" \
      -C "${AGENTX_CONFIG_DB}" \
      -c "${AGENTX_CACHE_DB}" \
      -s "${AGENTX_IPC_SOCKET}" \
      "$@"
    ;;
  telemetry)
    exec /usr/local/bin/telemetry \
      --cache-dir "${AGENTX_CACHE_DB}" \
      --config-dir "${AGENTX_CONFIG_DB}" \
      "$@"
    ;;
  *)
    # Allow arbitrary commands too (e.g. a shell for debugging), rather than
    # rejecting anything that isn't one of the two known binaries.
    exec "${cmd}" "$@"
    ;;
esac
