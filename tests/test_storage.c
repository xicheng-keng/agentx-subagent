/*
 * test_storage.c — plain C exerciser for storage_lmdb.c.
 * Exits 0 on success (all checks passed), non-zero on first failure.
 * Prints one line per check.
 */
#include "storage_lmdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("PASS: %s\n", (msg)); \
        } else { \
            printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            g_failures++; \
        } \
    } while (0)

#define CHECK_RC(rc, expected, msg) \
    CHECK((rc) == (expected), msg)

static char g_tmpdir[4096];

static void rm_rf(const char *path)
{
    /* Try to remove as a directory first (recurse into contents). */
    DIR *d = opendir(path);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            char child[8192];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            rm_rf(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        /* Not a directory (or can't open it) — try unlinking as a file. */
        unlink(path);
    }
}

static void make_tmpdir(void)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/storage_test_XXXXXX", base);
    if (mkdtemp(g_tmpdir) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
}

static void test_roundtrip_types(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_roundtrip", g_tmpdir);

    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: env_open persistent");

    /* int32 */
    rc = storage_set_int(env, "i.neg", -12345);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set int32 negative");
    int32_t iv = 0;
    rc = storage_get_int(env, "i.neg", &iv);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get int32 negative");
    CHECK(iv == -12345, "roundtrip: int32 negative value matches");

    rc = storage_set_int(env, "i.min", INT32_MIN);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set int32 INT32_MIN");
    rc = storage_get_int(env, "i.min", &iv);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get int32 INT32_MIN");
    CHECK(iv == INT32_MIN, "roundtrip: int32 INT32_MIN value matches");

    rc = storage_set_int(env, "i.max", INT32_MAX);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set int32 INT32_MAX");
    rc = storage_get_int(env, "i.max", &iv);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get int32 INT32_MAX");
    CHECK(iv == INT32_MAX, "roundtrip: int32 INT32_MAX value matches");

    /* uint32 */
    uint32_t uv = 0;
    rc = storage_set_uint(env, "u.max", UINT32_MAX);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set uint32 UINT32_MAX");
    rc = storage_get_uint(env, "u.max", &uv);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get uint32 UINT32_MAX");
    CHECK(uv == UINT32_MAX, "roundtrip: uint32 UINT32_MAX value matches");

    rc = storage_set_uint(env, "u.zero", 0);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set uint32 zero");
    rc = storage_get_uint(env, "u.zero", &uv);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get uint32 zero");
    CHECK(uv == 0, "roundtrip: uint32 zero value matches");

    /* uint64 */
    uint64_t u64v = 0;
    uint64_t big64 = 0xFFFFFFFFFFFFFFFFULL;
    rc = storage_set_u64(env, "u64.max", big64);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set uint64 max");
    rc = storage_get_u64(env, "u64.max", &u64v);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get uint64 max");
    CHECK(u64v == big64, "roundtrip: uint64 max value matches");

    /* bytes, zero-length */
    rc = storage_set_bytes(env, "b.empty", "", 0);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set bytes zero-length");
    char smallbuf[8];
    size_t blen = sizeof(smallbuf);
    rc = storage_get_bytes(env, "b.empty", smallbuf, &blen);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get bytes zero-length");
    CHECK(blen == 0, "roundtrip: bytes zero-length length matches");

    /* bytes, binary with embedded NULs */
    unsigned char binval[8] = { 0x00, 0x01, 0x00, 0xFF, 0x10, 0x00, 0x7F, 0x00 };
    rc = storage_set_bytes(env, "b.bin", binval, sizeof(binval));
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set bytes binary with NULs");
    unsigned char binbuf[8];
    size_t binlen = sizeof(binbuf);
    rc = storage_get_bytes(env, "b.bin", binbuf, &binlen);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get bytes binary with NULs");
    CHECK(binlen == sizeof(binval) && memcmp(binbuf, binval, sizeof(binval)) == 0,
          "roundtrip: bytes binary content matches");

    /* oid */
    uint32_t oidval[] = { 1, 3, 6, 1, 4, 1, 99999 };
    rc = storage_set_oid(env, "o.test", oidval, 7);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: set oid");
    uint32_t oidbuf[16];
    size_t oidcount = 16;
    rc = storage_get_oid(env, "o.test", oidbuf, &oidcount);
    CHECK_RC(rc, STORAGE_OK, "roundtrip: get oid");
    CHECK(oidcount == 7 && memcmp(oidbuf, oidval, sizeof(oidval)) == 0,
          "roundtrip: oid content matches");

    storage_env_close(env);
}

static void test_missing_and_type_mismatch(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_missing", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "missing: env_open");

    int32_t iv;
    rc = storage_get_int(env, "does.not.exist", &iv);
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "missing: get on absent key returns NOTFOUND");

    rc = storage_set_int(env, "typed.key", 42);
    CHECK_RC(rc, STORAGE_OK, "missing: set int for type-mismatch test");
    uint32_t uv;
    rc = storage_get_uint(env, "typed.key", &uv);
    CHECK_RC(rc, STORAGE_ERR_TYPE, "missing: get uint on int key returns TYPE mismatch");

    storage_env_close(env);
}

static void test_bytes_undersized_buffer(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_undersized", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "undersized: env_open");

    const char *payload = "hello world, this is a longer string";
    size_t payload_len = strlen(payload);
    rc = storage_set_bytes(env, "big.bytes", payload, payload_len);
    CHECK_RC(rc, STORAGE_OK, "undersized: set bytes");

    char smallbuf[4];
    size_t len = sizeof(smallbuf);
    rc = storage_get_bytes(env, "big.bytes", smallbuf, &len);
    CHECK_RC(rc, STORAGE_ERR_TOOBIG, "undersized: get bytes with small buffer returns TOOBIG");
    CHECK(len == payload_len, "undersized: *len updated to required length");

    char *bigbuf = (char *)malloc(len);
    size_t len2 = len;
    rc = storage_get_bytes(env, "big.bytes", bigbuf, &len2);
    CHECK_RC(rc, STORAGE_OK, "undersized: retry with correctly sized buffer succeeds");
    CHECK(len2 == payload_len && memcmp(bigbuf, payload, payload_len) == 0,
          "undersized: retried content matches");
    free(bigbuf);

    storage_env_close(env);
}

static void test_overwrite(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_overwrite", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "overwrite: env_open");

    rc = storage_set_int(env, "over.key", 1);
    CHECK_RC(rc, STORAGE_OK, "overwrite: initial set");
    rc = storage_set_int(env, "over.key", 2);
    CHECK_RC(rc, STORAGE_OK, "overwrite: second set");
    int32_t iv;
    rc = storage_get_int(env, "over.key", &iv);
    CHECK_RC(rc, STORAGE_OK, "overwrite: get after overwrite");
    CHECK(iv == 2, "overwrite: value reflects last write");

    storage_env_close(env);
}

static void test_delete(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_delete", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "delete: env_open");

    rc = storage_set_int(env, "del.key", 7);
    CHECK_RC(rc, STORAGE_OK, "delete: set key");
    rc = storage_delete(env, "del.key");
    CHECK_RC(rc, STORAGE_OK, "delete: delete existing key");
    int32_t iv;
    rc = storage_get_int(env, "del.key", &iv);
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "delete: get after delete returns NOTFOUND");

    rc = storage_delete(env, "del.key");
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "delete: delete of missing key returns NOTFOUND");

    storage_env_close(env);
}

static void test_exists_type_of(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_exists", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "exists: env_open");

    rc = storage_exists(env, "no.such.key");
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "exists: missing key reports NOTFOUND");

    rc = storage_set_uint(env, "exists.key", 123);
    CHECK_RC(rc, STORAGE_OK, "exists: set key");
    rc = storage_exists(env, "exists.key");
    CHECK_RC(rc, STORAGE_OK, "exists: existing key reports OK");

    storage_type_t t;
    rc = storage_type_of(env, "exists.key", &t);
    CHECK_RC(rc, STORAGE_OK, "exists: type_of succeeds");
    CHECK(t == STORAGE_TYPE_UINT32, "exists: type_of reports UINT32");

    storage_env_close(env);
}

static void test_explicit_txn(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_txn", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "txn: env_open");

    /* committed multi-key txn */
    storage_txn_t *txn = NULL;
    rc = storage_txn_begin(env, 0, &txn);
    CHECK_RC(rc, STORAGE_OK, "txn: begin write txn");
    rc = storage_txn_set_int(txn, "txn.a", 1);
    CHECK_RC(rc, STORAGE_OK, "txn: set txn.a");
    rc = storage_txn_set_int(txn, "txn.b", 2);
    CHECK_RC(rc, STORAGE_OK, "txn: set txn.b");
    rc = storage_txn_commit(txn);
    CHECK_RC(rc, STORAGE_OK, "txn: commit");

    int32_t iv;
    rc = storage_get_int(env, "txn.a", &iv);
    CHECK(rc == STORAGE_OK && iv == 1, "txn: txn.a committed and readable");
    rc = storage_get_int(env, "txn.b", &iv);
    CHECK(rc == STORAGE_OK && iv == 2, "txn: txn.b committed and readable");

    /* aborted multi-key txn leaves no keys behind */
    storage_txn_t *txn2 = NULL;
    rc = storage_txn_begin(env, 0, &txn2);
    CHECK_RC(rc, STORAGE_OK, "txn: begin second write txn");
    rc = storage_txn_set_int(txn2, "txn.aborted1", 100);
    CHECK_RC(rc, STORAGE_OK, "txn: set txn.aborted1 in aborted txn");
    rc = storage_txn_set_int(txn2, "txn.aborted2", 200);
    CHECK_RC(rc, STORAGE_OK, "txn: set txn.aborted2 in aborted txn");
    storage_txn_abort(txn2);

    rc = storage_exists(env, "txn.aborted1");
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "txn: aborted key1 not present");
    rc = storage_exists(env, "txn.aborted2");
    CHECK_RC(rc, STORAGE_ERR_NOTFOUND, "txn: aborted key2 not present");

    storage_env_close(env);
}

static void test_readonly_env(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_readonly", g_tmpdir);

    storage_env_t *rw = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &rw);
    CHECK_RC(rc, STORAGE_OK, "readonly: rw env_open");
    rc = storage_set_int(rw, "ro.key", 55);
    CHECK_RC(rc, STORAGE_OK, "readonly: rw writes value");
    storage_env_close(rw);

    storage_env_t *ro = NULL;
    rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT | STORAGE_ENV_READONLY, &ro);
    CHECK_RC(rc, STORAGE_OK, "readonly: ro env_open");
    CHECK(storage_env_is_readonly(ro) != 0, "readonly: is_readonly reports true");

    rc = storage_set_int(ro, "ro.key", 66);
    CHECK_RC(rc, STORAGE_ERR_READONLY, "readonly: write on ro env rejected");

    int32_t iv;
    rc = storage_get_int(ro, "ro.key", &iv);
    CHECK(rc == STORAGE_OK && iv == 55, "readonly: ro env reads value written by rw handle");

    storage_env_close(ro);
}

static void test_key_too_long(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_longkey", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "longkey: env_open");

    char longkey[STORAGE_KEY_MAX + 2];
    memset(longkey, 'k', sizeof(longkey) - 1);
    longkey[sizeof(longkey) - 1] = '\0';

    rc = storage_set_int(env, longkey, 1);
    CHECK_RC(rc, STORAGE_ERR_TOOBIG, "longkey: over-long key rejected with TOOBIG");

    storage_env_close(env);
}

static void test_nosync_env(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_nosync", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_NOSYNC, &env);
    CHECK_RC(rc, STORAGE_OK, "nosync: env_open with NOSYNC");

    rc = storage_set_int(env, "ns.key", 999);
    CHECK_RC(rc, STORAGE_OK, "nosync: set works identically");
    int32_t iv;
    rc = storage_get_int(env, "ns.key", &iv);
    CHECK(rc == STORAGE_OK && iv == 999, "nosync: get works identically");

    rc = storage_env_sync(env, 1);
    CHECK_RC(rc, STORAGE_OK, "nosync: forced sync succeeds");

    storage_env_close(env);
}

static void test_env_stat(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_stat", g_tmpdir);
    storage_env_t *env = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env);
    CHECK_RC(rc, STORAGE_OK, "stat: env_open");

    uint64_t entries = 12345, bytes_used = 0;
    rc = storage_env_stat(env, &entries, &bytes_used);
    CHECK_RC(rc, STORAGE_OK, "stat: stat on empty env succeeds");
    CHECK(entries == 0, "stat: empty env reports zero entries");

    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "stat.key.%d", i);
        rc = storage_set_int(env, key, i);
        CHECK_RC(rc, STORAGE_OK, "stat: seed key set succeeds");
    }

    rc = storage_env_stat(env, &entries, &bytes_used);
    CHECK_RC(rc, STORAGE_OK, "stat: stat after inserts succeeds");
    CHECK(entries == 10, "stat: entry count reflects inserted keys");
    CHECK(bytes_used > 0, "stat: bytes_used is plausible (nonzero)");

    storage_env_close(env);
}

static void test_readonly_write_txn_rejected(void)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/env_txn_ro", g_tmpdir);
    storage_env_t *rw = NULL;
    storage_rc_t rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &rw);
    CHECK_RC(rc, STORAGE_OK, "txn_ro: rw env_open");
    storage_env_close(rw);

    storage_env_t *ro = NULL;
    rc = storage_env_open(path, 0, STORAGE_ENV_PERSISTENT | STORAGE_ENV_READONLY, &ro);
    CHECK_RC(rc, STORAGE_OK, "txn_ro: ro env_open");

    storage_txn_t *txn = NULL;
    rc = storage_txn_begin(ro, 0, &txn);
    CHECK_RC(rc, STORAGE_ERR_READONLY, "txn_ro: write txn on ro env rejected at begin");

    storage_txn_t *rtxn = NULL;
    rc = storage_txn_begin(ro, 1, &rtxn);
    CHECK_RC(rc, STORAGE_OK, "txn_ro: read txn on ro env succeeds");
    storage_txn_abort(rtxn);

    storage_env_close(ro);
}

int main(void)
{
    make_tmpdir();

    test_roundtrip_types();
    test_missing_and_type_mismatch();
    test_bytes_undersized_buffer();
    test_overwrite();
    test_delete();
    test_exists_type_of();
    test_explicit_txn();
    test_readonly_env();
    test_key_too_long();
    test_nosync_env();
    test_env_stat();
    test_readonly_write_txn_rejected();

    rm_rf(g_tmpdir);

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
}
