/*
 * subagent_env.h — process wide LMDB environment handles.
 *
 * Generated MIB handlers reference `config_env` and `cache_env` directly;
 * they differ only by which handle is passed to the storage_* helpers.
 */
#ifndef SUBAGENT_ENV_H
#define SUBAGENT_ENV_H

#include "storage_lmdb.h"

#ifdef __cplusplus
extern "C" {
#endif

extern storage_env_t *config_env;   /* persistent, fsync, subagent is writer */
extern storage_env_t *cache_env;    /* volatile on tmpfs, MDB_NOSYNC         */

/* Default paths; overridable with AGENTX_CONFIG_DB / AGENTX_CACHE_DB. */
#define SUBAGENT_CONFIG_DB_DEFAULT "/var/lib/agentx-subagent/config.lmdb"
#define SUBAGENT_CACHE_DB_DEFAULT  "/run/agentx-subagent/cache.lmdb"

/*
 * Open both environments and, when the cache environment is empty (tmpfs was
 * just remounted), populate it via cache_bootstrap_defaults().
 * Returns 0 on success, -1 on failure (details are logged via snmp_log).
 */
int  subagent_storage_init(const char *config_path, const char *cache_path);
void subagent_storage_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* SUBAGENT_ENV_H */
