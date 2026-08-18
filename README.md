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

## Layout

| Path | Contents |
| --- | --- |
| `include/` | Cross-component contracts: storage API, value encoding, storage mode switches, IPC and trap interfaces |
| `src/storage_lmdb.c` | The LMDB abstraction both environments share |
| `src/generated/` | mib2c output: MIB handlers and the cache bootstrap (do not hand-edit) |
| `src/ipc_server.c` | Length-prefixed protobuf server on AF_UNIX |
| `src/main.c`, `src/demo_trap.c` | Subagent entry point, event loop and notifications |
| `mib2c/` | Custom mib2c templates |
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

Keys are the MIB object name in ASCII (`"sampleIntervalSec"`), with the
instance appended for table cells (`"ifAdminStatusExt.3"`).

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
multiplexing and malformed input, and GET load. The simulated power-loss
scenario needs device-mapper and skips with a message where that is
unavailable — it reports a skip, never a pass.

## Continuous integration

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request:

- **C tests** — builds against the matched LMDB (see [Build](#build) above,
  not the distro `liblmdb-dev`), runs `ctest`, and runs both test binaries
  under `valgrind --leak-check=full`. The build uses `-Werror` so a new
  compiler warning fails CI.
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

The C and Rust jobs both build the C side via `scripts/build-matched-lmdb.sh`
rather than the distro LMDB package: the two must link the exact same LMDB
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
