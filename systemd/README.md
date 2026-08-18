# systemd units

Start-up ordering is a correctness requirement, not a convenience
(docs/design.md 2.3): if either process opens `cache.lmdb` before the tmpfs is
mounted and seeded, it will serve or overwrite values that should not exist.

    agentx-cache.mount            tmpfs at /run/agentx-subagent
      └── agentx-subagent.service opens config.lmdb, creates + seeds cache.lmdb
            └── agentx-telemetry.service  starts only once seeding is done

The mount unit's file name must match its `Where=` path escaped by
`systemd-escape -p --suffix=mount /run/agentx-subagent`, i.e.
`run-agentx\x2dsubagent.mount`. `agentx-cache.mount` here is the readable
source; install it under the escaped name:

    install -m0644 systemd/agentx-cache.mount \
        /etc/systemd/system/'run-agentx\x2dsubagent.mount'

Create the service account before enabling the units:

    useradd --system --no-create-home --shell /usr/sbin/nologin agentx
