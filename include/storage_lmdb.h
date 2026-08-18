/*
 * storage_lmdb.h — thin LMDB abstraction shared by all generated MIB handlers.
 *
 * Design contract (docs/design.md ch.2):
 *   - Two LMDB environments with identical API, differing only by handle:
 *       config_env : persistent, fsync enabled  (writer: C subagent only)
 *       cache_env  : volatile on tmpfs, MDB_NOSYNC (writer: Rust app, plus
 *                    C subagent for writable-but-volatile MIB objects)
 *   - Values are stored with a self-describing 4-byte header so that the C
 *     subagent and the Rust application agree on the wire format byte for byte.
 *
 * Value encoding (little endian, identical on both sides):
 *   offset 0      : uint8_t  type tag (storage_type_t)
 *   offset 1..3   : uint8_t  reserved, MUST be written as 0
 *   offset 4..    : payload
 *                     STORAGE_TYPE_INT32  : int32_t
 *                     STORAGE_TYPE_UINT32 : uint32_t
 *                     STORAGE_TYPE_UINT64 : uint64_t
 *                     STORAGE_TYPE_BYTES  : raw bytes, length = value_len - 4
 *                     STORAGE_TYPE_OID    : n * uint32_t sub-identifiers
 *   The 4-byte header keeps the payload naturally aligned inside the mmap.
 *
 * Key encoding:
 *   ASCII MIB object name, no trailing NUL, e.g. "tempThreshold".
 *   Table cells append '.' and the row's instance sub-identifiers in dotted
 *   decimal, exactly as they appear on the wire after the column OID:
 *   "portDescr.3" for a single integer index, "fooBar.2.7" for a two
 *   sub-identifier instance. Because the instance is taken verbatim from the
 *   OID, no index type needs to be understood by the storage layer; see
 *   include/table_rows.h for the build/parse helpers the generated table
 *   handlers use, and docs/design.md 3.2.
 */
#ifndef STORAGE_LMDB_H
#define STORAGE_LMDB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_VALUE_HEADER_LEN 4u
#define STORAGE_KEY_MAX          255u   /* LMDB default max key size is 511 */

typedef enum {
    STORAGE_TYPE_INVALID = 0,
    STORAGE_TYPE_INT32   = 1,
    STORAGE_TYPE_UINT32  = 2,
    STORAGE_TYPE_UINT64  = 3,
    STORAGE_TYPE_BYTES   = 4,
    STORAGE_TYPE_OID     = 5
} storage_type_t;

typedef enum {
    STORAGE_OK           =  0,
    STORAGE_ERR_NOTFOUND = -1,  /* key absent */
    STORAGE_ERR_TYPE     = -2,  /* stored type tag != requested type */
    STORAGE_ERR_INVAL    = -3,  /* bad argument / malformed stored value */
    STORAGE_ERR_TOOBIG   = -4,  /* caller buffer too small, or key too long */
    STORAGE_ERR_FULL     = -5,  /* MDB_MAP_FULL / MDB_TXN_FULL */
    STORAGE_ERR_BUSY     = -6,  /* writer lock contended (non-blocking paths) */
    STORAGE_ERR_IO       = -7,  /* any other LMDB/OS failure */
    STORAGE_ERR_READONLY = -8   /* write attempted on a read-only env handle */
} storage_rc_t;

/* Environment open flags (bitmask, independent of LMDB's own flags). */
#define STORAGE_ENV_PERSISTENT 0x00u /* fsync on commit (config.lmdb)      */
#define STORAGE_ENV_NOSYNC     0x01u /* MDB_NOSYNC        (cache.lmdb)     */
#define STORAGE_ENV_READONLY   0x02u /* MDB_RDONLY, writes are rejected    */
#define STORAGE_ENV_NOSUBDIR   0x04u /* path is the data file, not a dir   */

typedef struct storage_env storage_env_t;
typedef struct storage_txn storage_txn_t;

/* --- environment lifecycle ------------------------------------------- */

/*
 * Open (creating if needed) an LMDB environment.
 * path     : directory (default) or file when STORAGE_ENV_NOSUBDIR is set
 * mapsize  : maximum map size in bytes; 0 selects STORAGE_DEFAULT_MAPSIZE
 * flags    : STORAGE_ENV_* bitmask
 * Returns STORAGE_OK and sets *out, or an error with *out untouched.
 */
storage_rc_t storage_env_open(const char *path, size_t mapsize,
                              unsigned flags, storage_env_t **out);
void         storage_env_close(storage_env_t *env);
int          storage_env_is_readonly(const storage_env_t *env);
/* Force an fsync; no-op semantics are still safe on NOSYNC environments. */
storage_rc_t storage_env_sync(storage_env_t *env, int force);

#define STORAGE_DEFAULT_MAPSIZE (64u * 1024u * 1024u)

/* --- single-operation helpers (own transaction per call) -------------- */

storage_rc_t storage_get_int(storage_env_t *env, const char *key, int32_t *out);
storage_rc_t storage_set_int(storage_env_t *env, const char *key, int32_t val);
storage_rc_t storage_get_uint(storage_env_t *env, const char *key, uint32_t *out);
storage_rc_t storage_set_uint(storage_env_t *env, const char *key, uint32_t val);
storage_rc_t storage_get_u64(storage_env_t *env, const char *key, uint64_t *out);
storage_rc_t storage_set_u64(storage_env_t *env, const char *key, uint64_t val);

/*
 * Byte-string accessors. storage_get_bytes copies at most *len bytes into buf
 * and updates *len with the stored length. If the stored value is longer than
 * the supplied buffer, STORAGE_ERR_TOOBIG is returned and *len is set to the
 * length required.
 */
storage_rc_t storage_get_bytes(storage_env_t *env, const char *key,
                               void *buf, size_t *len);
storage_rc_t storage_set_bytes(storage_env_t *env, const char *key,
                               const void *buf, size_t len);

storage_rc_t storage_get_oid(storage_env_t *env, const char *key,
                             uint32_t *subids, size_t *count);
storage_rc_t storage_set_oid(storage_env_t *env, const char *key,
                             const uint32_t *subids, size_t count);

storage_rc_t storage_delete(storage_env_t *env, const char *key);
storage_rc_t storage_exists(storage_env_t *env, const char *key);
/* Report the stored type tag without decoding the payload. */
storage_rc_t storage_type_of(storage_env_t *env, const char *key,
                             storage_type_t *out);

/* --- explicit transactions (multi-key atomicity, e.g. bootstrap) ------ */

storage_rc_t storage_txn_begin(storage_env_t *env, int readonly,
                               storage_txn_t **out);
storage_rc_t storage_txn_commit(storage_txn_t *txn);
void         storage_txn_abort(storage_txn_t *txn);

storage_rc_t storage_txn_get_int(storage_txn_t *txn, const char *key, int32_t *out);
storage_rc_t storage_txn_set_int(storage_txn_t *txn, const char *key, int32_t val);
storage_rc_t storage_txn_get_uint(storage_txn_t *txn, const char *key, uint32_t *out);
storage_rc_t storage_txn_set_uint(storage_txn_t *txn, const char *key, uint32_t val);
storage_rc_t storage_txn_get_u64(storage_txn_t *txn, const char *key, uint64_t *out);
storage_rc_t storage_txn_set_u64(storage_txn_t *txn, const char *key, uint64_t val);
storage_rc_t storage_txn_get_bytes(storage_txn_t *txn, const char *key,
                                   void *buf, size_t *len);
storage_rc_t storage_txn_set_bytes(storage_txn_t *txn, const char *key,
                                   const void *buf, size_t len);
storage_rc_t storage_txn_delete(storage_txn_t *txn, const char *key);

/* --- prefix iteration (conceptual table row discovery) ---------------- */

typedef struct storage_iter storage_iter_t;

/*
 * Open a read-only cursor over every key that starts with `prefix`, in LMDB's
 * (byte-wise ascending) key order. A NULL or empty prefix walks the whole
 * database.
 *
 * The iterator pins a read transaction for its entire lifetime. LMDB cannot
 * reuse pages that a live reader might still see, so a long lived iterator
 * makes the map grow: open it, drain it, close it, and never hold one across
 * an event loop iteration.
 */
storage_rc_t storage_iter_open(storage_env_t *env, const char *prefix,
                               storage_iter_t **out);

/*
 * Advance to the next matching key. On STORAGE_OK, *key points at the key
 * bytes inside the LMDB map -- not NUL terminated, and only valid until the
 * next storage_iter_next() or storage_iter_close() call on this iterator --
 * and *keylen is its length. Returns STORAGE_ERR_NOTFOUND, leaving *key and
 * *keylen untouched, once the prefix range is exhausted.
 */
storage_rc_t storage_iter_next(storage_iter_t *it, const char **key,
                               size_t *keylen);

/* Closes the cursor and ends the pinned read transaction. NULL is a no-op. */
void storage_iter_close(storage_iter_t *it);

/* --- diagnostics ------------------------------------------------------ */

const char *storage_strerror(storage_rc_t rc);
/* Report the entry count and page bytes from mdb_stat/mdb_env_info. */
storage_rc_t storage_env_stat(storage_env_t *env, uint64_t *entries,
                              uint64_t *bytes_used);

#ifdef __cplusplus
}
#endif
#endif /* STORAGE_LMDB_H */
