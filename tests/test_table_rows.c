/*
 * test_table_rows.c — plain C exerciser for src/table_rows.c and the
 * prefix-iteration API it is built on (storage_iter_* in storage_lmdb.c).
 *
 * This is the layer that decides which rows of a conceptual table exist
 * (docs/design.md 3.2), so the interesting cases are the ones a MIB walk
 * would otherwise get subtly wrong: numeric versus bytewise ordering, a row
 * that only some columns have a cell for, cells split across the two
 * environments, and keys a foreign writer left in a shape that is not a row
 * instance at all.
 *
 * Exits 0 on success (all checks passed), non-zero on first failure.
 * Prints one line per check.
 */
#include "storage_lmdb.h"
#include "table_rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

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
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
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
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void make_tmpdir(void)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }
    snprintf(g_tmpdir, sizeof(g_tmpdir), "%s/table_rows_test_XXXXXX", base);
    if (mkdtemp(g_tmpdir) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(1);
    }
}

static storage_env_t *open_env(const char *name)
{
    char path[8192];
    storage_env_t *env = NULL;

    snprintf(path, sizeof(path), "%s/%s", g_tmpdir, name);
    if (storage_env_open(path, 0, STORAGE_ENV_PERSISTENT, &env) != STORAGE_OK) {
        fprintf(stderr, "storage_env_open(%s) failed\n", path);
        exit(1);
    }
    return env;
}

static table_instance_t inst1(uint32_t a)
{
    table_instance_t inst;

    memset(&inst, 0, sizeof(inst));
    inst.subid[0] = a;
    inst.len = 1;
    return inst;
}

static table_instance_t inst2(uint32_t a, uint32_t b)
{
    table_instance_t inst = inst1(a);

    inst.subid[1] = b;
    inst.len = 2;
    return inst;
}

/* --------------------------------------------------------------------- */
/* instance parsing / formatting                                         */
/* --------------------------------------------------------------------- */

static void test_instance_parse_accepts(void)
{
    table_instance_t inst;

    CHECK_RC(table_instance_parse("3", 1, &inst), STORAGE_OK, "parse: '3' ok");
    CHECK(inst.len == 1 && inst.subid[0] == 3, "parse: '3' -> [3]");

    CHECK_RC(table_instance_parse("2.7", 3, &inst), STORAGE_OK, "parse: '2.7' ok");
    CHECK(inst.len == 2 && inst.subid[0] == 2 && inst.subid[1] == 7,
          "parse: '2.7' -> [2,7]");

    CHECK_RC(table_instance_parse("0", 1, &inst), STORAGE_OK, "parse: '0' ok");
    CHECK(inst.len == 1 && inst.subid[0] == 0, "parse: '0' -> [0]");

    CHECK_RC(table_instance_parse("4294967295", 10, &inst), STORAGE_OK,
             "parse: 2^32-1 ok");
    CHECK(inst.subid[0] == 4294967295u, "parse: 2^32-1 value");

    /* The caller passes a length, not a NUL terminated string: parsing must
     * stop at it, since keys come straight out of the LMDB map. */
    CHECK_RC(table_instance_parse("12.34", 2, &inst), STORAGE_OK,
             "parse: honours the length argument");
    CHECK(inst.len == 1 && inst.subid[0] == 12, "parse: '12.34'[0..2) -> [12]");
}

static void test_instance_parse_rejects(void)
{
    table_instance_t inst;

    CHECK_RC(table_instance_parse("", 0, &inst), STORAGE_ERR_INVAL,
             "parse: empty rejected");
    CHECK_RC(table_instance_parse("007", 3, &inst), STORAGE_ERR_INVAL,
             "parse: leading zeros rejected (would alias row 7)");
    CHECK_RC(table_instance_parse("1.02", 4, &inst), STORAGE_ERR_INVAL,
             "parse: leading zeros in a later component rejected");
    CHECK_RC(table_instance_parse("abc", 3, &inst), STORAGE_ERR_INVAL,
             "parse: non-numeric rejected");
    CHECK_RC(table_instance_parse("1.", 2, &inst), STORAGE_ERR_INVAL,
             "parse: trailing dot rejected");
    CHECK_RC(table_instance_parse(".1", 2, &inst), STORAGE_ERR_INVAL,
             "parse: leading dot rejected");
    CHECK_RC(table_instance_parse("1..2", 4, &inst), STORAGE_ERR_INVAL,
             "parse: empty component rejected");
    CHECK_RC(table_instance_parse("1 2", 3, &inst), STORAGE_ERR_INVAL,
             "parse: stray character rejected");
    CHECK_RC(table_instance_parse("4294967296", 10, &inst), STORAGE_ERR_INVAL,
             "parse: 2^32 rejected (not a sub-identifier)");
    CHECK_RC(table_instance_parse("-1", 2, &inst), STORAGE_ERR_INVAL,
             "parse: negative rejected");
}

static void test_instance_format_and_key(void)
{
    char buf[STORAGE_KEY_MAX + 1];
    table_instance_t inst = inst2(2, 7);

    CHECK_RC(table_instance_format(&inst, buf, sizeof(buf)), STORAGE_OK,
             "format: [2,7] ok");
    CHECK(strcmp(buf, "2.7") == 0, "format: [2,7] -> '2.7'");

    CHECK_RC(table_cell_key("portDescr", &inst, buf, sizeof(buf)), STORAGE_OK,
             "cell_key: ok");
    CHECK(strcmp(buf, "portDescr.2.7") == 0,
          "cell_key: 'portDescr' + [2,7] -> 'portDescr.2.7'");

    /* Round trip: what a key is built from is what parsing it gives back. */
    {
        table_instance_t back;
        const char *dotted = strchr(buf, '.') + 1;

        CHECK_RC(table_instance_parse(dotted, strlen(dotted), &back), STORAGE_OK,
                 "cell_key: instance parses back");
        CHECK(table_instance_compare(&inst, &back) == 0,
              "cell_key: round trip is lossless");
    }

    {
        char small[4];

        CHECK_RC(table_cell_key("portDescr", &inst, small, sizeof(small)),
                 STORAGE_ERR_TOOBIG, "cell_key: undersized buffer rejected");
    }
    {
        char longcol[STORAGE_KEY_MAX + 8];

        memset(longcol, 'c', sizeof(longcol) - 1);
        longcol[sizeof(longcol) - 1] = '\0';
        CHECK_RC(table_cell_key(longcol, &inst, buf, sizeof(buf)),
                 STORAGE_ERR_TOOBIG, "cell_key: over-long column rejected");
    }
    {
        table_instance_t empty;

        memset(&empty, 0, sizeof(empty));
        CHECK_RC(table_instance_format(&empty, buf, sizeof(buf)),
                 STORAGE_ERR_INVAL, "format: empty instance rejected");
    }
}

static void test_instance_compare(void)
{
    table_instance_t a = inst1(2);
    table_instance_t b = inst1(10);
    table_instance_t c = inst2(2, 1);

    CHECK(table_instance_compare(&a, &b) < 0,
          "compare: 2 < 10 numerically (not bytewise, where '10' < '2')");
    CHECK(table_instance_compare(&b, &a) > 0, "compare: 10 > 2");
    CHECK(table_instance_compare(&a, &a) == 0, "compare: equal instances");
    CHECK(table_instance_compare(&a, &c) < 0,
          "compare: a prefix sorts before the longer instance");
    CHECK(table_instance_compare(&c, &b) < 0, "compare: 2.1 < 10");
}

/* --------------------------------------------------------------------- */
/* prefix iteration                                                      */
/* --------------------------------------------------------------------- */

static void test_prefix_iteration(void)
{
    storage_env_t *env = open_env("env_iter");
    storage_iter_t *it = NULL;
    const char *key = NULL;
    size_t keylen = 0;
    int seen = 0;

    /* Two columns whose names share a prefix, plus a scalar, so the scan has
     * something to wrongly pick up if the '.' were not part of the prefix. */
    CHECK_RC(storage_set_int(env, "portDescr.1", 1), STORAGE_OK, "iter: seed 1");
    CHECK_RC(storage_set_int(env, "portDescr.2", 2), STORAGE_OK, "iter: seed 2");
    CHECK_RC(storage_set_int(env, "portDescrExtra.1", 3), STORAGE_OK, "iter: seed 3");
    CHECK_RC(storage_set_int(env, "portDescr", 4), STORAGE_OK, "iter: seed 4");
    CHECK_RC(storage_set_int(env, "zzz.1", 5), STORAGE_OK, "iter: seed 5");

    CHECK_RC(storage_iter_open(env, "portDescr.", &it), STORAGE_OK,
             "iter: open with prefix");
    while (storage_iter_next(it, &key, &keylen) == STORAGE_OK) {
        CHECK(keylen > 10 && strncmp(key, "portDescr.", 10) == 0,
              "iter: key carries the prefix");
        seen++;
    }
    storage_iter_close(it);
    CHECK(seen == 2, "iter: exactly the two prefixed keys (no scalar, no sibling column)");

    /* Exhausted iterators stay exhausted rather than wrapping around. */
    CHECK_RC(storage_iter_open(env, "nothing.", &it), STORAGE_OK,
             "iter: open on an empty range");
    CHECK_RC(storage_iter_next(it, &key, &keylen), STORAGE_ERR_NOTFOUND,
             "iter: empty range reports NOTFOUND");
    CHECK_RC(storage_iter_next(it, &key, &keylen), STORAGE_ERR_NOTFOUND,
             "iter: still NOTFOUND after exhaustion");
    storage_iter_close(it);

    seen = 0;
    CHECK_RC(storage_iter_open(env, NULL, &it), STORAGE_OK,
             "iter: open with no prefix");
    while (storage_iter_next(it, &key, &keylen) == STORAGE_OK) {
        seen++;
    }
    storage_iter_close(it);
    CHECK(seen == 5, "iter: no prefix walks the whole database");

    storage_iter_close(NULL); /* documented no-op */
    storage_env_close(env);
}

/* --------------------------------------------------------------------- */
/* rowsets                                                               */
/* --------------------------------------------------------------------- */

static void test_rowset_union_sort_dedupe(void)
{
    storage_env_t *config = open_env("env_rows_config");
    storage_env_t *cache = open_env("env_rows_cache");
    table_rowset_t *rows = NULL;

    /*
     * A table split across both environments, as storage_mode.h allows:
     * portDescr/portAdminStatus persistent, portAlarmThresholdMilliC
     * volatile. Row 2 exists only in the cache environment, row 3 only has a
     * descr (a hole in the other columns), and row 10 is there to catch a
     * bytewise sort.
     */
    storage_set_bytes(config, "portDescr.1", "a", 1);
    storage_set_int(config, "portAdminStatus.1", 1);
    storage_set_int(cache, "portAlarmThresholdMilliC.1", 80000);
    storage_set_int(cache, "portAlarmThresholdMilliC.2", 80000);
    storage_set_bytes(config, "portDescr.3", "c", 1);
    storage_set_bytes(config, "portDescr.10", "j", 1);

    {
        const table_column_ref_t cols[] = {
            { config, "portDescr" },
            { config, "portAdminStatus" },
            { cache,  "portAlarmThresholdMilliC" },
        };

        CHECK_RC(table_rowset_load(cols, 3, &rows), STORAGE_OK, "rowset: load");
    }

    CHECK(table_rowset_count(rows) == 4,
          "rowset: union of both environments, duplicates collapsed");
    CHECK(table_rowset_skipped(rows) == 0, "rowset: nothing skipped");
    CHECK(table_rowset_at(rows, 0)->subid[0] == 1, "rowset: [0] == row 1");
    CHECK(table_rowset_at(rows, 1)->subid[0] == 2,
          "rowset: [1] == row 2 (cache-only column)");
    CHECK(table_rowset_at(rows, 2)->subid[0] == 3, "rowset: [2] == row 3");
    CHECK(table_rowset_at(rows, 3)->subid[0] == 10,
          "rowset: [3] == row 10, i.e. numeric order, not '10' < '2'");
    CHECK(table_rowset_at(rows, 4) == NULL, "rowset: out of range yields NULL");

    table_rowset_free(rows);
    storage_env_close(config);
    storage_env_close(cache);
}

static void test_rowset_malformed_and_empty(void)
{
    storage_env_t *env = open_env("env_rows_malformed");
    table_rowset_t *rows = NULL;
    const table_column_ref_t cols[] = { { env, "sensorName" } };

    CHECK_RC(table_rowset_load(cols, 1, &rows), STORAGE_OK,
             "rowset: empty table loads");
    CHECK(table_rowset_count(rows) == 0, "rowset: empty table has no rows");
    table_rowset_free(rows);

    storage_set_bytes(env, "sensorName.1", "ok", 2);
    storage_set_bytes(env, "sensorName.007", "leading zero", 12);
    storage_set_bytes(env, "sensorName.abc", "not a number", 12);
    storage_set_bytes(env, "sensorName.", "no instance at all", 18);

    CHECK_RC(table_rowset_load(cols, 1, &rows), STORAGE_OK,
             "rowset: load with malformed keys present");
    CHECK(table_rowset_count(rows) == 1,
          "rowset: only the well formed key becomes a row");
    CHECK(table_rowset_skipped(rows) == 3, "rowset: the other three are counted");
    table_rowset_free(rows);

    /* A bad argument must not be reported as an empty table. */
    {
        const table_column_ref_t bad[] = { { env, "" } };

        CHECK_RC(table_rowset_load(bad, 1, &rows), STORAGE_ERR_INVAL,
                 "rowset: empty column name rejected");
        CHECK_RC(table_rowset_load(cols, 0, &rows), STORAGE_ERR_INVAL,
                 "rowset: zero columns rejected");
    }

    CHECK(table_rowset_count(NULL) == 0, "rowset: count(NULL) is 0");
    CHECK(table_rowset_at(NULL, 0) == NULL, "rowset: at(NULL) is NULL");
    table_rowset_free(NULL); /* documented no-op */

    storage_env_close(env);
}

static void test_rowset_multi_subid_instances(void)
{
    storage_env_t *env = open_env("env_rows_multi");
    table_rowset_t *rows = NULL;
    const table_column_ref_t cols[] = { { env, "cell" } };

    /* Nothing in this layer knows how many sub-identifiers an index has, so
     * a two-object index has to come back in OID order just the same. */
    storage_set_int(env, "cell.2.10", 1);
    storage_set_int(env, "cell.2.2", 2);
    storage_set_int(env, "cell.1.9", 3);

    CHECK_RC(table_rowset_load(cols, 1, &rows), STORAGE_OK,
             "rowset: multi sub-identifier load");
    CHECK(table_rowset_count(rows) == 3, "rowset: three composite rows");
    CHECK(table_rowset_at(rows, 0)->len == 2 &&
          table_rowset_at(rows, 0)->subid[0] == 1 &&
          table_rowset_at(rows, 0)->subid[1] == 9, "rowset: [0] == 1.9");
    CHECK(table_rowset_at(rows, 1)->subid[0] == 2 &&
          table_rowset_at(rows, 1)->subid[1] == 2, "rowset: [1] == 2.2");
    CHECK(table_rowset_at(rows, 2)->subid[0] == 2 &&
          table_rowset_at(rows, 2)->subid[1] == 10, "rowset: [2] == 2.10");

    table_rowset_free(rows);
    storage_env_close(env);
}

static void test_rowset_many_rows(void)
{
    storage_env_t *env = open_env("env_rows_many");
    table_rowset_t *rows = NULL;
    const table_column_ref_t cols[] = { { env, "wide" } };
    const uint32_t total = 500; /* forces the rowset to grow past its initial capacity */
    uint32_t i;
    int ordered = 1;

    for (i = 1; i <= total; i++) {
        char key[STORAGE_KEY_MAX + 1];
        table_instance_t inst = inst1(i);

        if (table_cell_key("wide", &inst, key, sizeof(key)) != STORAGE_OK ||
            storage_set_uint(env, key, i) != STORAGE_OK) {
            break;
        }
    }
    CHECK(i == total + 1, "rowset: seeded 500 rows");

    CHECK_RC(table_rowset_load(cols, 1, &rows), STORAGE_OK, "rowset: load 500 rows");
    CHECK(table_rowset_count(rows) == total, "rowset: all 500 rows present");
    for (i = 0; i < table_rowset_count(rows); i++) {
        if (table_rowset_at(rows, i)->subid[0] != i + 1) {
            ordered = 0;
            break;
        }
    }
    CHECK(ordered, "rowset: 500 rows come back in ascending numeric order");

    table_rowset_free(rows);
    storage_env_close(env);
}

int main(void)
{
    make_tmpdir();

    test_instance_parse_accepts();
    test_instance_parse_rejects();
    test_instance_format_and_key();
    test_instance_compare();
    test_prefix_iteration();
    test_rowset_union_sort_dedupe();
    test_rowset_malformed_and_empty();
    test_rowset_multi_subid_instances();
    test_rowset_many_rows();

    rm_rf(g_tmpdir);

    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
}
