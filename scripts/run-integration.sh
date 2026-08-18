#!/usr/bin/env bash
# run-integration.sh -- driver for the cross-process integration scenarios
# under tests/ (docs/design.md ch.5.3).
#
# Runs every tests/scenario_*.sh in order, in its own subshell (so one
# scenario's failure/trap cleanup can never leak into the next), and prints
# a final PASS/FAIL/SKIP table. Exits nonzero iff at least one scenario
# genuinely FAILed (a SKIP -- a scenario that detected a missing binary or
# missing privilege and bailed out loudly -- is not a failure).
#
# Usage:
#   scripts/run-integration.sh                 # run every scenario_*.sh
#   scripts/run-integration.sh scenario_a ...   # run only the named ones
#   ITEST_BUILD_DIR=/path/to/build scripts/run-integration.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TESTS_DIR="${REPO_ROOT}/tests"

# shellcheck disable=SC2034  # exported for the scenario scripts to inherit
export ITEST_BUILD_DIR="${ITEST_BUILD_DIR:-${REPO_ROOT}/build-it}"

mapfile -t ALL_SCENARIOS < <(find "${TESTS_DIR}" -maxdepth 1 -name 'scenario_*.sh' | sort)

if [[ $# -gt 0 ]]; then
  SCENARIOS=()
  for pat in "$@"; do
    for s in "${ALL_SCENARIOS[@]}"; do
      [[ "$(basename "${s}")" == *"${pat}"* ]] && SCENARIOS+=("${s}")
    done
  done
else
  SCENARIOS=("${ALL_SCENARIOS[@]}")
fi

if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
  echo "run-integration: no matching scenario scripts found" >&2
  exit 2
fi

declare -A RESULT
declare -A DURATION_S
overall_rc=0

for s in "${SCENARIOS[@]}"; do
  name="$(basename "${s}" .sh)"
  echo
  echo "=================================================================="
  echo "==  running ${name}"
  echo "=================================================================="
  log_file="$(mktemp "/tmp/agentx-it-run-${name}.XXXXXX.log")"
  t0=$(date +%s)
  if bash "${s}" 2>&1 | tee "${log_file}"; then
    rc=0
  else
    rc=$?
  fi
  t1=$(date +%s)
  DURATION_S["${name}"]=$((t1 - t0))

  if grep -q "^\[.*\] SKIP:" "${log_file}"; then
    RESULT["${name}"]="SKIP"
  elif [[ ${rc} -eq 0 ]]; then
    RESULT["${name}"]="PASS"
  else
    RESULT["${name}"]="FAIL"
    overall_rc=1
  fi
  # A failed scenario's log is the only durable record of what went wrong once
  # the run ends, so keep it (CI uploads these as an artifact). Passing and
  # skipping scenarios leave nothing behind.
  if [[ "${RESULT[${name}]}" == "FAIL" ]]; then
    echo "[run-integration] keeping log for failed scenario: ${log_file}"
  else
    rm -f "${log_file}"
  fi
done

echo
echo "=================================================================="
echo "==  summary"
echo "=================================================================="
printf '%-40s %-6s %s\n' "scenario" "result" "seconds"
for s in "${SCENARIOS[@]}"; do
  name="$(basename "${s}" .sh)"
  printf '%-40s %-6s %s\n' "${name}" "${RESULT[${name}]}" "${DURATION_S[${name}]}"
done

exit "${overall_rc}"
