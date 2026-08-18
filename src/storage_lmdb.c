/*
 * storage_lmdb.c — implementation of the thin LMDB abstraction declared in
 * include/storage_lmdb.h. See that header for the full design contract
 * (value encoding, key encoding, error mapping). This file must not change
 * any of the header's declarations, value encoding, or return codes.
 */
#include "storage_lmdb.h"

#include <lmdb.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

struct storage_env {
    MDB_env *env;
    MDB_dbi  dbi;
    unsigned flags;      /* STORAGE_ENV_* bitmask as passed to storage_env_open */
    int      readonly;   /* 1 if STORAGE_ENV_READONLY was set */
};

struct storage_txn {
    storage_env_t *env;
    MDB_txn       *txn;
    int            readonly;
};

/* --------------------------------------------------------------------- */
/* little-endian byte helpers (correct on big-endian hosts too)          */
/* --------------------------------------------------------------------- */

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64le(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

static uint64_t get_u64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* --------------------------------------------------------------------- */
/* error mapping                                                         */
/* --------------------------------------------------------------------- */

static storage_rc_t map_mdb_rc(int rc)
{
    if (rc == MDB_SUCCESS) {
        return STORAGE_OK;
    }
    switch (rc) {
    case MDB_NOTFOUND:
        return STORAGE_ERR_NOTFOUND;
    case MDB_MAP_FULL:
    case MDB_TXN_FULL:
        return STORAGE_ERR_FULL;
    case EINVAL:
        return STORAGE_ERR_INVAL;
    case EACCES:
        return STORAGE_ERR_READONLY;
    case EAGAIN:
    case EBUSY:
        return STORAGE_ERR_BUSY;
    default:
        return STORAGE_ERR_IO;
    }
}

/* --------------------------------------------------------------------- */
/* key validation                                                        */
/* --------------------------------------------------------------------- */

static storage_rc_t validate_key(const char *key, size_t *keylen_out)
{
    if (key == NULL) {
        return STORAGE_ERR_INVAL;
    }
    size_t len = strlen(key);
    if (len == 0) {
        return STORAGE_ERR_INVAL;
    }
    if (len > STORAGE_KEY_MAX) {
        return STORAGE_ERR_TOOBIG;
    }
    *keylen_out = len;
    return STORAGE_OK;
}

/* --------------------------------------------------------------------- */
/* environment lifecycle                                                 */
/* --------------------------------------------------------------------- */

storage_rc_t storage_env_open(const char *path, size_t mapsize,
                               unsigned flags, storage_env_t **out)
{
    if (path == NULL || out == NULL) {
        return STORAGE_ERR_INVAL;
    }

    if (!(flags & STORAGE_ENV_NOSUBDIR)) {
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            return STORAGE_ERR_IO;
        }
    }

    MDB_env *menv = NULL;
    int rc = mdb_env_create(&menv);
    if (rc != MDB_SUCCESS) {
        return map_mdb_rc(rc);
    }

    size_t effective_mapsize = (mapsize != 0) ? mapsize : STORAGE_DEFAULT_MAPSIZE;
    rc = mdb_env_set_mapsize(menv, effective_mapsize);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(menv);
        return map_mdb_rc(rc);
    }

    rc = mdb_env_set_maxreaders(menv, 126);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(menv);
        return map_mdb_rc(rc);
    }

    /* We use a single unnamed database, but it still must be opened via
     * mdb_dbi_open() at least once before use; that requires at least one
     * named-database slot to be reserved (default 0 is fine for the
     * unnamed DB). */

    unsigned mdb_flags = 0;
    if (flags & STORAGE_ENV_NOSYNC) {
        mdb_flags |= MDB_NOSYNC;
    }
    if (flags & STORAGE_ENV_READONLY) {
        mdb_flags |= MDB_RDONLY;
    }
    if (flags & STORAGE_ENV_NOSUBDIR) {
        mdb_flags |= MDB_NOSUBDIR;
    }

    rc = mdb_env_open(menv, path, mdb_flags, 0644);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(menv);
        return map_mdb_rc(rc);
    }

    /* Open the (unnamed) dbi once, up front, inside a short transaction,
     * so later read-only transactions never need MDB_CREATE. */
    MDB_txn *txn = NULL;
    unsigned txn_flags = (flags & STORAGE_ENV_READONLY) ? MDB_RDONLY : 0;
    rc = mdb_txn_begin(menv, NULL, txn_flags, &txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(menv);
        return map_mdb_rc(rc);
    }

    MDB_dbi dbi;
    unsigned dbi_flags = (flags & STORAGE_ENV_READONLY) ? 0 : MDB_CREATE;
    rc = mdb_dbi_open(txn, NULL, dbi_flags, &dbi);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(menv);
        return map_mdb_rc(rc);
    }

    if (flags & STORAGE_ENV_READONLY) {
        mdb_txn_abort(txn);
    } else {
        rc = mdb_txn_commit(txn);
        if (rc != MDB_SUCCESS) {
            mdb_env_close(menv);
            return map_mdb_rc(rc);
        }
    }

    storage_env_t *senv = (storage_env_t *)calloc(1, sizeof(*senv));
    if (senv == NULL) {
        mdb_env_close(menv);
        return STORAGE_ERR_IO;
    }
    senv->env = menv;
    senv->dbi = dbi;
    senv->flags = flags;
    senv->readonly = (flags & STORAGE_ENV_READONLY) ? 1 : 0;

    *out = senv;
    return STORAGE_OK;
}

void storage_env_close(storage_env_t *env)
{
    if (env == NULL) {
        return;
    }
    if (env->env != NULL) {
        mdb_env_close(env->env);
    }
    free(env);
}

int storage_env_is_readonly(const storage_env_t *env)
{
    if (env == NULL) {
        return 0;
    }
    return env->readonly;
}

storage_rc_t storage_env_sync(storage_env_t *env, int force)
{
    if (env == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (env->readonly) {
        return STORAGE_OK;
    }
    int rc = mdb_env_sync(env->env, force);
    if (rc != MDB_SUCCESS) {
        return map_mdb_rc(rc);
    }
    return STORAGE_OK;
}

/* --------------------------------------------------------------------- */
/* common codec helpers                                                  */
/* --------------------------------------------------------------------- */

/* Decode a stored MDB_val according to expected_type. On success, *payload
 * points inside mdb_data (valid only while the owning txn is live) and
 * *payload_len is the payload length (mdb_data length - header length). */
static storage_rc_t decode_header(const MDB_val *mdb_data,
                                   storage_type_t expected_type,
                                   const uint8_t **payload, size_t *payload_len)
{
    if (mdb_data->mv_size < STORAGE_VALUE_HEADER_LEN) {
        return STORAGE_ERR_INVAL;
    }
    const uint8_t *bytes = (const uint8_t *)mdb_data->mv_data;
    storage_type_t stored_type = (storage_type_t)bytes[0];
    if (stored_type != expected_type) {
        return STORAGE_ERR_TYPE;
    }
    *payload = bytes + STORAGE_VALUE_HEADER_LEN;
    *payload_len = mdb_data->mv_size - STORAGE_VALUE_HEADER_LEN;
    return STORAGE_OK;
}

static void encode_header(uint8_t *buf, storage_type_t type)
{
    buf[0] = (uint8_t)type;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 0;
}

/* --------------------------------------------------------------------- */
/* internal get/set/delete/exists/type_of, operating on an existing txn  */
/* --------------------------------------------------------------------- */

static storage_rc_t txn_get_raw(MDB_txn *txn, MDB_dbi dbi, const char *key,
                                 MDB_val *out_data)
{
    size_t keylen;
    storage_rc_t rc = validate_key(key, &keylen);
    if (rc != STORAGE_OK) {
        return rc;
    }
    MDB_val mkey;
    mkey.mv_size = keylen;
    mkey.mv_data = (void *)key;
    int mrc = mdb_get(txn, dbi, &mkey, out_data);
    if (mrc != MDB_SUCCESS) {
        return map_mdb_rc(mrc);
    }
    return STORAGE_OK;
}

static storage_rc_t txn_put_raw(MDB_txn *txn, MDB_dbi dbi, const char *key,
                                 const void *data, size_t data_len)
{
    size_t keylen;
    storage_rc_t rc = validate_key(key, &keylen);
    if (rc != STORAGE_OK) {
        return rc;
    }
    MDB_val mkey, mval;
    mkey.mv_size = keylen;
    mkey.mv_data = (void *)key;
    mval.mv_size = data_len;
    mval.mv_data = (void *)data;
    int mrc = mdb_put(txn, dbi, &mkey, &mval, 0);
    if (mrc != MDB_SUCCESS) {
        return map_mdb_rc(mrc);
    }
    return STORAGE_OK;
}

static storage_rc_t txn_get_int_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, int32_t *out)
{
    if (out == NULL) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    const uint8_t *payload;
    size_t payload_len;
    rc = decode_header(&data, STORAGE_TYPE_INT32, &payload, &payload_len);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (payload_len < sizeof(int32_t)) {
        return STORAGE_ERR_INVAL;
    }
    *out = (int32_t)get_u32le(payload);
    return STORAGE_OK;
}

static storage_rc_t txn_set_int_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, int32_t val)
{
    uint8_t buf[STORAGE_VALUE_HEADER_LEN + sizeof(int32_t)];
    encode_header(buf, STORAGE_TYPE_INT32);
    put_u32le(buf + STORAGE_VALUE_HEADER_LEN, (uint32_t)val);
    return txn_put_raw(txn, dbi, key, buf, sizeof(buf));
}

static storage_rc_t txn_get_uint_impl(MDB_txn *txn, MDB_dbi dbi,
                                       const char *key, uint32_t *out)
{
    if (out == NULL) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    const uint8_t *payload;
    size_t payload_len;
    rc = decode_header(&data, STORAGE_TYPE_UINT32, &payload, &payload_len);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (payload_len < sizeof(uint32_t)) {
        return STORAGE_ERR_INVAL;
    }
    *out = get_u32le(payload);
    return STORAGE_OK;
}

static storage_rc_t txn_set_uint_impl(MDB_txn *txn, MDB_dbi dbi,
                                       const char *key, uint32_t val)
{
    uint8_t buf[STORAGE_VALUE_HEADER_LEN + sizeof(uint32_t)];
    encode_header(buf, STORAGE_TYPE_UINT32);
    put_u32le(buf + STORAGE_VALUE_HEADER_LEN, val);
    return txn_put_raw(txn, dbi, key, buf, sizeof(buf));
}

static storage_rc_t txn_get_u64_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, uint64_t *out)
{
    if (out == NULL) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    const uint8_t *payload;
    size_t payload_len;
    rc = decode_header(&data, STORAGE_TYPE_UINT64, &payload, &payload_len);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (payload_len < sizeof(uint64_t)) {
        return STORAGE_ERR_INVAL;
    }
    *out = get_u64le(payload);
    return STORAGE_OK;
}

static storage_rc_t txn_set_u64_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, uint64_t val)
{
    uint8_t buf[STORAGE_VALUE_HEADER_LEN + sizeof(uint64_t)];
    encode_header(buf, STORAGE_TYPE_UINT64);
    put_u64le(buf + STORAGE_VALUE_HEADER_LEN, val);
    return txn_put_raw(txn, dbi, key, buf, sizeof(buf));
}

static storage_rc_t txn_get_bytes_impl(MDB_txn *txn, MDB_dbi dbi,
                                        const char *key, void *buf, size_t *len)
{
    if (len == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (buf == NULL && *len != 0) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    const uint8_t *payload;
    size_t payload_len;
    rc = decode_header(&data, STORAGE_TYPE_BYTES, &payload, &payload_len);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (payload_len > *len) {
        *len = payload_len;
        return STORAGE_ERR_TOOBIG;
    }
    if (payload_len > 0) {
        memcpy(buf, payload, payload_len);
    }
    *len = payload_len;
    return STORAGE_OK;
}

static storage_rc_t txn_set_bytes_impl(MDB_txn *txn, MDB_dbi dbi,
                                        const char *key, const void *buf,
                                        size_t len)
{
    if (buf == NULL && len != 0) {
        return STORAGE_ERR_INVAL;
    }
    size_t total = (size_t)STORAGE_VALUE_HEADER_LEN + len;
    uint8_t *tmp = (uint8_t *)malloc(total > 0 ? total : 1);
    if (tmp == NULL) {
        return STORAGE_ERR_IO;
    }
    encode_header(tmp, STORAGE_TYPE_BYTES);
    if (len > 0) {
        memcpy(tmp + STORAGE_VALUE_HEADER_LEN, buf, len);
    }
    storage_rc_t rc = txn_put_raw(txn, dbi, key, tmp, total);
    free(tmp);
    return rc;
}

static storage_rc_t txn_get_oid_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, uint32_t *subids,
                                      size_t *count)
{
    if (count == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (subids == NULL && *count != 0) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    const uint8_t *payload;
    size_t payload_len;
    rc = decode_header(&data, STORAGE_TYPE_OID, &payload, &payload_len);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (payload_len % sizeof(uint32_t) != 0) {
        return STORAGE_ERR_INVAL;
    }
    size_t n = payload_len / sizeof(uint32_t);
    if (n > *count) {
        *count = n;
        return STORAGE_ERR_TOOBIG;
    }
    for (size_t i = 0; i < n; i++) {
        subids[i] = get_u32le(payload + i * sizeof(uint32_t));
    }
    *count = n;
    return STORAGE_OK;
}

static storage_rc_t txn_set_oid_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, const uint32_t *subids,
                                      size_t count)
{
    if (subids == NULL && count != 0) {
        return STORAGE_ERR_INVAL;
    }
    size_t total = (size_t)STORAGE_VALUE_HEADER_LEN + count * sizeof(uint32_t);
    uint8_t *tmp = (uint8_t *)malloc(total > 0 ? total : 1);
    if (tmp == NULL) {
        return STORAGE_ERR_IO;
    }
    encode_header(tmp, STORAGE_TYPE_OID);
    for (size_t i = 0; i < count; i++) {
        put_u32le(tmp + STORAGE_VALUE_HEADER_LEN + i * sizeof(uint32_t), subids[i]);
    }
    storage_rc_t rc = txn_put_raw(txn, dbi, key, tmp, total);
    free(tmp);
    return rc;
}

static storage_rc_t txn_delete_impl(MDB_txn *txn, MDB_dbi dbi, const char *key)
{
    size_t keylen;
    storage_rc_t rc = validate_key(key, &keylen);
    if (rc != STORAGE_OK) {
        return rc;
    }
    MDB_val mkey;
    mkey.mv_size = keylen;
    mkey.mv_data = (void *)key;
    int mrc = mdb_del(txn, dbi, &mkey, NULL);
    if (mrc != MDB_SUCCESS) {
        return map_mdb_rc(mrc);
    }
    return STORAGE_OK;
}

static storage_rc_t txn_exists_impl(MDB_txn *txn, MDB_dbi dbi, const char *key)
{
    MDB_val data;
    return txn_get_raw(txn, dbi, key, &data);
}

static storage_rc_t txn_type_of_impl(MDB_txn *txn, MDB_dbi dbi,
                                      const char *key, storage_type_t *out)
{
    if (out == NULL) {
        return STORAGE_ERR_INVAL;
    }
    MDB_val data;
    storage_rc_t rc = txn_get_raw(txn, dbi, key, &data);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (data.mv_size < STORAGE_VALUE_HEADER_LEN) {
        return STORAGE_ERR_INVAL;
    }
    *out = (storage_type_t)((const uint8_t *)data.mv_data)[0];
    return STORAGE_OK;
}

/* --------------------------------------------------------------------- */
/* single-operation helpers: each opens its own txn                      */
/* --------------------------------------------------------------------- */

static storage_rc_t begin_read_txn(storage_env_t *env, MDB_txn **out)
{
    int rc = mdb_txn_begin(env->env, NULL, MDB_RDONLY, out);
    return map_mdb_rc(rc);
}

static storage_rc_t begin_write_txn(storage_env_t *env, MDB_txn **out)
{
    if (env->readonly) {
        return STORAGE_ERR_READONLY;
    }
    int rc = mdb_txn_begin(env->env, NULL, 0, out);
    return map_mdb_rc(rc);
}

static storage_rc_t commit_write_txn(MDB_txn *txn)
{
    int rc = mdb_txn_commit(txn);
    return map_mdb_rc(rc);
}

#define STORAGE_ENV_CHECK(env) \
    do { \
        if ((env) == NULL) { \
            return STORAGE_ERR_INVAL; \
        } \
    } while (0)

storage_rc_t storage_get_int(storage_env_t *env, const char *key, int32_t *out)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_get_int_impl(txn, env->dbi, key, out);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_set_int(storage_env_t *env, const char *key, int32_t val)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_set_int_impl(txn, env->dbi, key, val);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_get_uint(storage_env_t *env, const char *key, uint32_t *out)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_get_uint_impl(txn, env->dbi, key, out);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_set_uint(storage_env_t *env, const char *key, uint32_t val)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_set_uint_impl(txn, env->dbi, key, val);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_get_u64(storage_env_t *env, const char *key, uint64_t *out)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_get_u64_impl(txn, env->dbi, key, out);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_set_u64(storage_env_t *env, const char *key, uint64_t val)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_set_u64_impl(txn, env->dbi, key, val);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_get_bytes(storage_env_t *env, const char *key,
                                void *buf, size_t *len)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_get_bytes_impl(txn, env->dbi, key, buf, len);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_set_bytes(storage_env_t *env, const char *key,
                                const void *buf, size_t len)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_set_bytes_impl(txn, env->dbi, key, buf, len);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_get_oid(storage_env_t *env, const char *key,
                              uint32_t *subids, size_t *count)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_get_oid_impl(txn, env->dbi, key, subids, count);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_set_oid(storage_env_t *env, const char *key,
                              const uint32_t *subids, size_t count)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_set_oid_impl(txn, env->dbi, key, subids, count);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_delete(storage_env_t *env, const char *key)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_write_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_delete_impl(txn, env->dbi, key);
    if (rc != STORAGE_OK) {
        mdb_txn_abort(txn);
        return rc;
    }
    return commit_write_txn(txn);
}

storage_rc_t storage_exists(storage_env_t *env, const char *key)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_exists_impl(txn, env->dbi, key);
    mdb_txn_abort(txn);
    return rc;
}

storage_rc_t storage_type_of(storage_env_t *env, const char *key,
                              storage_type_t *out)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    rc = txn_type_of_impl(txn, env->dbi, key, out);
    mdb_txn_abort(txn);
    return rc;
}

/* --------------------------------------------------------------------- */
/* explicit transactions                                                 */
/* --------------------------------------------------------------------- */

storage_rc_t storage_txn_begin(storage_env_t *env, int readonly,
                                storage_txn_t **out)
{
    if (env == NULL || out == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (!readonly && env->readonly) {
        return STORAGE_ERR_READONLY;
    }
    MDB_txn *mtxn;
    int rc = mdb_txn_begin(env->env, NULL, readonly ? MDB_RDONLY : 0, &mtxn);
    if (rc != MDB_SUCCESS) {
        return map_mdb_rc(rc);
    }
    storage_txn_t *stxn = (storage_txn_t *)calloc(1, sizeof(*stxn));
    if (stxn == NULL) {
        mdb_txn_abort(mtxn);
        return STORAGE_ERR_IO;
    }
    stxn->env = env;
    stxn->txn = mtxn;
    stxn->readonly = readonly;
    *out = stxn;
    return STORAGE_OK;
}

storage_rc_t storage_txn_commit(storage_txn_t *txn)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    int rc = mdb_txn_commit(txn->txn);
    free(txn);
    return map_mdb_rc(rc);
}

void storage_txn_abort(storage_txn_t *txn)
{
    if (txn == NULL) {
        return;
    }
    mdb_txn_abort(txn->txn);
    free(txn);
}

storage_rc_t storage_txn_get_int(storage_txn_t *txn, const char *key, int32_t *out)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    return txn_get_int_impl(txn->txn, txn->env->dbi, key, out);
}

storage_rc_t storage_txn_set_int(storage_txn_t *txn, const char *key, int32_t val)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (txn->readonly) {
        return STORAGE_ERR_READONLY;
    }
    return txn_set_int_impl(txn->txn, txn->env->dbi, key, val);
}

storage_rc_t storage_txn_get_uint(storage_txn_t *txn, const char *key, uint32_t *out)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    return txn_get_uint_impl(txn->txn, txn->env->dbi, key, out);
}

storage_rc_t storage_txn_set_uint(storage_txn_t *txn, const char *key, uint32_t val)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (txn->readonly) {
        return STORAGE_ERR_READONLY;
    }
    return txn_set_uint_impl(txn->txn, txn->env->dbi, key, val);
}

storage_rc_t storage_txn_get_u64(storage_txn_t *txn, const char *key, uint64_t *out)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    return txn_get_u64_impl(txn->txn, txn->env->dbi, key, out);
}

storage_rc_t storage_txn_set_u64(storage_txn_t *txn, const char *key, uint64_t val)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (txn->readonly) {
        return STORAGE_ERR_READONLY;
    }
    return txn_set_u64_impl(txn->txn, txn->env->dbi, key, val);
}

storage_rc_t storage_txn_get_bytes(storage_txn_t *txn, const char *key,
                                    void *buf, size_t *len)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    return txn_get_bytes_impl(txn->txn, txn->env->dbi, key, buf, len);
}

storage_rc_t storage_txn_set_bytes(storage_txn_t *txn, const char *key,
                                    const void *buf, size_t len)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (txn->readonly) {
        return STORAGE_ERR_READONLY;
    }
    return txn_set_bytes_impl(txn->txn, txn->env->dbi, key, buf, len);
}

storage_rc_t storage_txn_delete(storage_txn_t *txn, const char *key)
{
    if (txn == NULL) {
        return STORAGE_ERR_INVAL;
    }
    if (txn->readonly) {
        return STORAGE_ERR_READONLY;
    }
    return txn_delete_impl(txn->txn, txn->env->dbi, key);
}

/* --------------------------------------------------------------------- */
/* diagnostics                                                           */
/* --------------------------------------------------------------------- */

const char *storage_strerror(storage_rc_t rc)
{
    switch (rc) {
    case STORAGE_OK:            return "ok";
    case STORAGE_ERR_NOTFOUND:  return "key not found";
    case STORAGE_ERR_TYPE:      return "type mismatch";
    case STORAGE_ERR_INVAL:     return "invalid argument";
    case STORAGE_ERR_TOOBIG:    return "buffer or key too big";
    case STORAGE_ERR_FULL:      return "map or transaction full";
    case STORAGE_ERR_BUSY:      return "writer lock contended";
    case STORAGE_ERR_IO:        return "i/o error";
    case STORAGE_ERR_READONLY:  return "read-only environment";
    default:                    return "unknown storage error";
    }
}

storage_rc_t storage_env_stat(storage_env_t *env, uint64_t *entries,
                               uint64_t *bytes_used)
{
    STORAGE_ENV_CHECK(env);
    MDB_txn *txn;
    storage_rc_t rc = begin_read_txn(env, &txn);
    if (rc != STORAGE_OK) {
        return rc;
    }
    MDB_stat stat;
    int mrc = mdb_stat(txn, env->dbi, &stat);
    if (mrc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        return map_mdb_rc(mrc);
    }
    if (entries != NULL) {
        *entries = (uint64_t)stat.ms_entries;
    }
    if (bytes_used != NULL) {
        uint64_t pages = (uint64_t)stat.ms_branch_pages +
                          (uint64_t)stat.ms_leaf_pages +
                          (uint64_t)stat.ms_overflow_pages;
        *bytes_used = pages * (uint64_t)stat.ms_psize;
    }
    mdb_txn_abort(txn);
    return STORAGE_OK;
}
