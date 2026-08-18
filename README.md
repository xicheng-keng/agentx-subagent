# agentx-subagent

[![CI](https://github.com/xicheng-keng/agentx-subagent/actions/workflows/ci.yml/badge.svg)](https://github.com/xicheng-keng/agentx-subagent/actions/workflows/ci.yml)

An AgentX (RFC 2741) subagent written in C against the net-snmp native API,
backed by two LMDB environments — one persistent, one volatile — and paired
with a Rust telemetry application that shares those environments.

The full design rationale is in [`docs/design.md`](docs/design.md). This README
covers the layout, how to build it, and how to run the checks.

## Architecture

```
[SNMP manager] --v1/v2c/v3--> [snmpd (master)] --AgentX/unix socket--> [C subagent]
                                                                          |
                                                +-------------------------+-------------------------+
                                                |                                                   |
                                        config.lmdb (persistent, fsync)              cache.lmdb (tmpfs, MDB_NOSYNC)
                                                |                                                   |
                                                +------------------ [Rust telemetry app] -----------+
                                                     read-only                    sole writer
```

SNMPv3 USM is handled entirely by `snmpd`; the subagent speaks only AgentX over
a local socket and never implements a v3 stack of its own.

### Single-writer rule

Every key has exactly one writer (design.md 2.5):

| Data | Environment | Writer |
| --- | --- | --- |
| SNMP-writable objects, persistent mode | `config.lmdb` | C subagent |
| SNMP-writable objects, volatile mode | `cache.lmdb` | C subagent |
| Telemetry / read-only objects | `cache.lmdb` | Rust app |

The Rust app opens `config.lmdb` read-only. When it needs a config value
changed it sends a `WriteConfigRequest` over the subagent's Unix socket and the
subagent performs the write, so the invariant holds without cross-process
locking.

### Storage backend per MIB object

`include/storage_mode.h` maps each read-write object to
`STORAGE_MODE_PERSISTENT` or `STORAGE_MODE_VOLATILE`. The generated handlers
contain both code paths and the preprocessor selects one; read-only objects are
generated against `cache.lmdb` only.

### Conceptual tables

Tables are handled one level down from scalars, with the same rule: the
backend is chosen **per column**, so a single row may straddle both
environments (`portConfigTable` in the demo MIB deliberately does). See
[`docs/design.md`](docs/design.md) 3.2 for the full rationale; in short:

- **Cells are ordinary keys.** `"<columnName>.<instance>"`, where the instance
  is the row's index sub-identifiers in dotted decimal, taken verbatim from
  the OID (`portDescr.3`). No index syntax is interpreted by the storage
  layer, so integer, string and multi-object indexes all work.
- **Rows are discovered, not declared.** Each request scans the key space for
  the table's column prefixes and takes the union: a row exists as long as one
  of its cells does. The result is sorted into OID order, which is *not* LMDB's
  key order — bytewise, `portDescr.10` precedes `portDescr.2`.
- **Configuration tables ship with rows.** A DEFVAL says what a cell of a row
  starts at, not that the row exists, so `include/table_provision.h` declares
  how many rows each writable table is provisioned with and the generated
  bootstrap seeds them (never overwriting an existing cell).
- **Telemetry tables do not.** `sensorTable` rows exist exactly while the Rust
  app keeps their cells present; a fresh boot reports an empty table rather
  than sensors that may not be there.
- **Managers cannot create or destroy rows.** There is no RowStatus column by
  design; a Set on a non-existent row is refused with `noCreation`.
- **A missing cell of an existing row** reports `noSuchInstance` and does not
  interrupt a walk. Writers should write a whole row in one transaction
  (`CacheStore::put_row` on the Rust side).

## Layout

| Path | Contents |
| --- | --- |
| `include/` | Cross-component contracts: storage API, value encoding, storage mode switches, table row provisioning, IPC and trap interfaces |
| `src/storage_lmdb.c` | The LMDB abstraction both environments share |
| `src/table_rows.c` | Table row discovery: cell keys, and the sorted row set behind every table request |
| `src/table_oid.c` | net-snmp OID <-> row instance conversions used by the generated table handlers |
| `src/generated/` | mib2c output: MIB handlers and the cache bootstrap (do not hand-edit) |
| `src/ipc_server.c` | Length-prefixed protobuf server on AF_UNIX |
| `src/main.c`, `src/demo_trap.c` | Subagent entry point, event loop and notifications |
| `mib2c/` | Custom mib2c templates (scalars, tables, bootstrap) |
| `mibs/AGENTX-DEMO-MIB.txt` | Demo MIB driving the whole exercise |
| `proto/` | IPC schema (nanopb on the C side, prost on the Rust side) |
| `rust-app/` | Telemetry application and IPC client |
| `tests/` | C unit tests and the cross-process integration scenarios |
| `docker/` | Multi-stage build / test / runtime images |
| `systemd/` | Units that enforce the tmpfs-before-open ordering |

## Value encoding

Both languages read and write the same bytes:

```
offset 0     type tag   1=int32 2=uint32 3=uint64 4=bytes 5=oid
offset 1..3  reserved, always zero
offset 4..   payload, little endian
```

Keys are the MIB object name in ASCII (`"sampleIntervalSec"`), with the row
instance appended for table cells (`"portDescr.3"`, `"sensorTempMilliC.2"`).

## Build

> **Both sides must link the same LMDB build.** `heed` always compiles its own
> vendored LMDB and cannot be pointed at the system library, so a C subagent
> linked against the distro's `liblmdb-dev` and a Rust app linked against the
> vendored copy will fail with `MDB_VERSION_MISMATCH` the moment they hold the
> same environment open at once. This does not show up in single-process
> testing. See `docs/design.md` appendix A.1.

```sh
sudo apt-get install -y liblmdb-dev libsnmp-dev snmpd snmp protobuf-compiler cmake

# Builds liblmdb from the same source Cargo vendors, and configures the C side
# against it. Use this rather than the distro package for any build whose
# binaries will share an environment with the Rust application.
scripts/build-matched-lmdb.sh

cmake --build build -j
(cd rust-app && cargo build --release)
```

## Test

```sh
ctest --test-dir build --output-on-failure   # C unit tests
(cd rust-app && cargo test)                  # Rust unit tests
scripts/run-integration.sh                   # cross-process scenarios
```

The integration suite covers the scenarios in `docs/design.md` 5.3: concurrent
writes under the single-writer rule, cache concurrency, subagent restart and
recovery, byte-level agreement between the two independent codecs, IPC
multiplexing and malformed input, GET load, and conceptual tables (row
discovery and ordering, a row split across both environments, multi-column
Sets, sparse rows, and rows appearing and disappearing under the agent). The
simulated power-loss scenario needs device-mapper and skips with a message
where that is unavailable — it reports a skip, never a pass.

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request:

- **C tests** — builds against the matched LMDB (see [Build](#build) above,
  not the distro `liblmdb-dev`), runs `ctest`, and runs the test binaries
  under `valgrind --leak-check=full` (table row discovery allocates per
  request, so a leak there would be per SNMP request). The build uses
  `-Werror` so a new compiler warning fails CI.
- **Rust** — `cargo fmt --check`, `cargo clippy -- -D warnings`, `cargo test`.
- **Generated code drift** — reruns `scripts/gen_mib2c.sh` and
  `scripts/gen_proto.sh` and fails if the committed `src/generated`/
  `src/generated_pb` no longer match their output, so template/schema edits
  can't silently leave stale generated code behind.
- **Integration** — builds both sides and runs
  `scripts/run-integration.sh`, the only check that exercises the C
  subagent and the Rust app against each other. Scenario g (`dm-flakey`)
  SKIPs on hosted runners for lack of a device-mapper kernel driver; that is
  expected and does not fail the job.
- **Docker** — builds all three `docker/Dockerfile` stages
  (`build`/`test`/`runtime`); never pushes an image.
- **shellcheck** — over `scripts/*.sh`, `tests/*.sh` and
  `docker/entrypoint.sh`.

The two jobs that compile C — **C tests** and **integration** — build it via
`scripts/build-matched-lmdb.sh` rather than against the distro LMDB package: the two must link the exact same LMDB
build or they fail with `MDB_VERSION_MISMATCH` the moment they share an
environment, a bug that only shows up cross-process (see `docs/design.md`
appendix A.1). Building against `liblmdb-dev` here would let CI stay green
on a configuration that breaks in the one scenario this project cares about.

## Regenerating

```sh
scripts/gen_mib2c.sh   # src/generated/ from mibs/ + mib2c/
scripts/gen_proto.sh   # src/generated_pb/ from proto/
```

Generated output is committed so that a build needs neither mib2c nor nanopb.
