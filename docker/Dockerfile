# syntax=docker/dockerfile:1
#
# Multi-stage build for agentx-subagent (docs/design.md ch.5.2):
#   build   -- net-snmp/lmdb dev headers + Rust toolchain; builds the C
#              subagent and the Rust telemetry app.
#   test    -- adds snmpd/snmp client tools/snmptrapd on top of `build`,
#              copies the integration scripts, and its CMD runs the
#              cross-process integration suite (docs/design.md 5.3).
#   runtime -- minimal deploy image: only the runtime shared libraries, the
#              two binaries, the MIB, and a non-root user. No compilers.
#
# *** Load-bearing detail: LMDB ABI/lock-file matching (see
#     scripts/build-matched-lmdb.sh for the full story) ***
#
# The Rust side (heed -> lmdb-master-sys) always compiles its own vendored
# LMDB from source and offers no option to link the distro's liblmdb; if the
# C subagent instead links the distro's `liblmdb-dev` package, the two
# binaries end up running *different* LMDB library builds against the same
# environment files. That is invisible in a single-process smoke test but
# breaks the moment both processes hold the environment open at the same
# time (`mdb_env_open` fails with MDB_VERSION_MISMATCH) -- exactly the
# concurrent, multi-process scenarios this whole test suite exists to check
# (docs/design.md 5.3). So this Dockerfile builds the C subagent against the
# *same* vendored LMDB source tree the Rust side pulled in, instead of the
# apt package, via scripts/build-matched-lmdb.sh.
#
# Build (from the repo root):
#   docker build -f docker/Dockerfile --target runtime -t agentx-subagent:runtime .
#   docker build -f docker/Dockerfile --target test    -t agentx-subagent:test .

ARG BASE_IMAGE=ubuntu:24.04

# =========================================================================
# build stage
# =========================================================================
FROM ${BASE_IMAGE} AS build
ENV DEBIAN_FRONTEND=noninteractive

# Dependency layer first (rarely changes) so source edits don't invalidate
# the apt/cargo download cache.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config \
      liblmdb-dev libsnmp-dev \
      protobuf-compiler \
      curl ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

# Optional extra CA certificate, for building behind a corporate/proxy MITM
# TLS setup (rustup's installer needs one such trust anchor in this repo's
# own sandboxed dev environment). Harmless and a no-op on a normal network:
# the wildcard COPY copies zero files if docker/extra-ca.crt isn't present,
# and update-ca-certificates then has nothing extra to add.
COPY docker/extra-ca.cr[t] /usr/local/share/ca-certificates/extra-ca.crt
RUN update-ca-certificates

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}
# rustup, not the distro rustc/cargo package: rust-app's dependency graph
# (heed/prost/clap at their pinned versions) and codec.rs's use of
# `is_multiple_of` need a materially newer toolchain than the LTS distro
# package (e.g. Ubuntu 24.04 ships rustc 1.75).
RUN set -e; \
    curl --proto '=https' --tlsv1.2 -fsSf https://sh.rustup.rs -o /tmp/rustup-init.sh; \
    sh /tmp/rustup-init.sh -y --profile minimal --default-toolchain stable; \
    rm -f /tmp/rustup-init.sh

WORKDIR /src

# --- Rust app: copy only its own manifests first for layer caching -------
COPY rust-app/Cargo.toml rust-app/Cargo.lock rust-app/build.rs /src/rust-app/
COPY proto/ /src/proto/
# rust-app/build.rs compiles proto/agentx_ipc.proto, so it needs a src/ tree
# to exist even for a dependency-only fetch; a placeholder lib.rs is enough.
RUN mkdir -p /src/rust-app/src/bin && \
    echo 'fn main() {}' > /src/rust-app/src/bin/telemetry.rs && \
    echo 'fn main() {}' > /src/rust-app/src/bin/loadgen.rs && \
    echo '// placeholder for the dependency-fetch layer' > /src/rust-app/src/lib.rs
WORKDIR /src/rust-app
RUN cargo fetch
# Now bring in the real sources and build for real.
COPY rust-app/src/ /src/rust-app/src/
RUN touch src/lib.rs src/bin/*.rs && cargo build --release

# --- C subagent: copy dependency-free headers/vendor first ---------------
WORKDIR /src
COPY include/ /src/include/
COPY third_party/ /src/third_party/
COPY mib2c/ /src/mib2c/
COPY mibs/ /src/mibs/
COPY scripts/ /src/scripts/
COPY CMakeLists.txt /src/CMakeLists.txt

# Build the LMDB the Rust side actually uses (see header comment above) and
# point CMake at it instead of the apt package's liblmdb-dev.
RUN bash scripts/build-matched-lmdb.sh /src/.lmdb-matched

COPY src/ /src/src/
COPY tests/ /src/tests/
RUN cmake -S /src -B /src/build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLMDB_INCLUDE_DIR=/src/.lmdb-matched/include \
      -DLMDB_LIBRARY=/src/.lmdb-matched/lib/libmdb-matched.a \
    && cmake --build /src/build -j

# it_helper: Rust-side helper CLI used by the bash integration scenarios.
WORKDIR /src/tests/rust_helpers
RUN cargo build --release

# =========================================================================
# test stage: build's artifacts + snmpd/snmp tools + the integration suite
# =========================================================================
FROM build AS test
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      snmpd snmp snmptrapd \
      lmdb-utils \
      procps iproute2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# scripts/, tests/ and the build-it dir are already present from the build
# stage; run-integration.sh derives the repo root from its own path and
# defaults ITEST_BUILD_DIR to <repo>/build-it, so point it at the build
# stage's actual output directory.
ENV ITEST_BUILD_DIR=/src/build

CMD ["bash", "scripts/run-integration.sh"]

# =========================================================================
# runtime stage: minimal deploy image, no compilers/toolchain
# =========================================================================
FROM ${BASE_IMAGE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive

# Only the runtime shared libraries agentx-subagent actually needs.
# liblmdb is statically linked (scripts/build-matched-lmdb.sh produces a
# .a), so no liblmdb runtime package is required here at all -- one fewer
# moving piece, and it sidesteps the ABI-matching concern entirely for the
# deployed binary.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libsnmp40t64 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system agentx \
    && useradd  --system --no-create-home --shell /usr/sbin/nologin -g agentx agentx

COPY --from=build /src/build/agentx-subagent /usr/local/bin/agentx-subagent
COPY --from=build /src/rust-app/target/release/telemetry /usr/local/bin/telemetry
COPY --from=build /src/mibs/AGENTX-DEMO-MIB.txt /usr/share/snmp/mibs/AGENTX-DEMO-MIB.txt
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# config.lmdb: persistent, must survive container restarts -> a real volume.
# cache.lmdb: MUST be tmpfs (docs/design.md 2.3) -- the volatile-reset
# semantics that keep it empty on every restart depend on the mountpoint
# itself disappearing, which a regular bind mount or named volume will NOT
# give you. Run this image with, e.g.:
#   docker run --tmpfs /run/agentx-subagent:mode=0750,uid=agentx,gid=agentx \
#              -v agentx-config:/var/lib/agentx-subagent \
#              agentx-subagent:runtime
# (docker-compose.yml in this directory sets this up via `tmpfs:`.)
VOLUME ["/var/lib/agentx-subagent"]
# Declaring /run/agentx-subagent as a VOLUME too, in addition to always
# running it with --tmpfs, means a caller who forgets the --tmpfs flag still
# gets an anonymous (empty-on-recreate, not persisted) volume rather than
# silently writing cache.lmdb into the container's own writable layer.
VOLUME ["/run/agentx-subagent"]

USER agentx
ENV AGENTX_CONFIG_DB=/var/lib/agentx-subagent/config.lmdb \
    AGENTX_CACHE_DB=/run/agentx-subagent/cache.lmdb \
    AGENTX_IPC_SOCKET=/run/agentx-subagent/ipc.sock \
    AGENTX_MASTER_SOCKET=unix:/var/agentx/master.sock

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["agentx-subagent"]
