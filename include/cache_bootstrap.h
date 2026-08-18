/*
 * cache_bootstrap.h — startup defaults for the volatile environment.
 *
 * src/generated/cache_bootstrap.c is produced by mib2c/mib2c.bootstrap.conf
 * from the MIB's DEFVAL clauses (docs/design.md ch.2.3).  tmpfs is empty on
 * every boot, so cache.lmdb must be seeded before either process serves a Get.
 */
#ifndef CACHE_BOOTSTRAP_H
#define CACHE_BOOTSTRAP_H

#include "storage_lmdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write every volatile object's default value in a single write transaction.
 * Existing keys are left untouched unless `overwrite` is non-zero, so a
 * subagent restart against a still-populated tmpfs does not clobber live
 * telemetry.
 */
storage_rc_t cache_bootstrap_defaults(storage_env_t *cache, int overwrite);

#ifdef __cplusplus
}
#endif
#endif /* CACHE_BOOTSTRAP_H */
