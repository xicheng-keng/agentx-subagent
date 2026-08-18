#!/usr/bin/env bash
#
# gen_mib2c.sh -- regenerate src/generated/*.{c,h} from AGENTX-DEMO-MIB
# using the project's custom mib2c templates (mib2c/mib2c.dbscalar.conf,
# mib2c/mib2c.bootstrap.conf). See docs/design.md chapter 3.1.
#
# Usage: scripts/gen_mib2c.sh
#
# Idempotent: re-running overwrites the generated files in src/generated/
# with fresh output. The generated files carry a header comment saying
# they must not be hand-edited -- this script is the only supposed way
# to produce them.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MIB2C_TEMPLATE_DIR="$ROOT_DIR/mib2c"
MIB_DIR="$ROOT_DIR/mibs"
OUT_DIR="$ROOT_DIR/src/generated"

command -v mib2c >/dev/null 2>&1 || {
    echo "error: mib2c not found in PATH (net-snmp-utils / net-snmp-perl not installed?)" >&2
    exit 1
}

mkdir -p "$OUT_DIR"

# mib2c (via the SNMP perl module) loads MIBS by name out of MIBDIRS.
export MIBS="+AGENTX-DEMO-MIB"
export MIBDIRS="+$MIB_DIR"

echo "== generating demo_config.{c,h} (demoConfig, read-write scalars) =="
(
    cd "$OUT_DIR"
    mib2c -I "$MIB2C_TEMPLATE_DIR" -c mib2c.dbscalar.conf -f demo_config -i demoConfig
)

echo "== generating demo_status.{c,h} (demoStatus, read-only scalars) =="
(
    cd "$OUT_DIR"
    mib2c -I "$MIB2C_TEMPLATE_DIR" -c mib2c.dbscalar.conf -f demo_status -i demoStatus
)

echo "== generating storage_bootstrap.c (cache.lmdb + config.lmdb DEFVAL bootstrap, whole MIB) =="
(
    cd "$OUT_DIR"
    # Run over the whole module so both demoConfig and demoStatus scalars
    # are visible to the bootstrap template in a single pass.
    mib2c -I "$MIB2C_TEMPLATE_DIR" -c mib2c.bootstrap.conf -i agentxDemoMIB
)

echo "== done: $OUT_DIR/demo_config.{c,h} $OUT_DIR/demo_status.{c,h} $OUT_DIR/storage_bootstrap.c =="
