/*
 * storage_bootstrap.h — startup defaults for both LMDB environments.
 *
 * src/generated/storage_bootstrap.c is produced by mib2c/mib2c.bootstrap.conf
 * from the MIB's DEFVAL clauses.
 *
 * Two environments, two reasons to seed:
 *
 *   cache.lmdb  lives on tmpfs and is therefore empty on every boot
 *               (docs/design.md 2.3). Seeding it is what makes a volatile
 *               object come back at its DEFVAL instead of at whatever the
 *               previous session left behind.
 *
 *   config.lmdb survives reboots, so it needs seeding exactly once, on a
 *               freshly provisioned device. Without it a manager walking a
 *               new unit gets NOSUCHINSTANCE for objects the MIB says exist,
 *               which is indistinguishable from a broken agent. Seeding is
 *               non-destructive: an existing key is never overwritten, so a
 *               firmware upgrade that adds MIB objects fills in only the new
 *               ones and leaves operator settings alone.
 */
#ifndef STORAGE_BOOTSTRAP_H
#define STORAGE_BOOTSTRAP_H

#include "storage_lmdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed the volatile environment's defaults in a single write transaction.
 * Existing keys are left untouched unless `overwrite` is non-zero, so a
 * subagent restart against a still-populated tmpfs does not clobber live
 * telemetry.
 */
storage_rc_t cache_bootstrap_defaults(storage_env_t *cache, int overwrite);

/*
 * Seed the persistent environment's defaults in a single write transaction.
 * Only keys that are absent are written, regardless of how many objects the
 * MIB gains over the product's life. There is deliberately no overwrite
 * parameter: overwriting persistent configuration would discard operator
 * intent, and nothing in the design has a legitimate reason to do that.
 */
storage_rc_t config_bootstrap_defaults(storage_env_t *config);

#ifdef __cplusplus
}
#endif
#endif /* STORAGE_BOOTSTRAP_H */
