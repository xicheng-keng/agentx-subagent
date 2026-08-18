/*
 * storage_cli.c -- small command line wrapper around storage_lmdb.h.
 *
 * Built only for the integration test harness under tests/ (see
 * CMakeLists.txt's AGENTX_BUILD_TESTS block). It gives the bash scenario
 * scripts in tests/ a way to:
 *
 *   - read/write LMDB values directly for concurrency and latency
 *     experiments that don't go through SNMP or the Rust app (e.g. a second
 *     writer contending for cache.lmdb in scenario (b));
 *   - repeatedly read a key and verify the decoded value is always
 *     well-formed (catches torn/partial reads, scenario (b));
 *   - fire a burst of GETs against config.lmdb/cache.lmdb and report
 *     latency percentiles (scenario (f));
 *   - dump a raw value's on-disk bytes as hex, for byte-for-byte
 *     cross-language comparison against the Rust side (scenario (d)).
 *
 * It intentionally never opens config.lmdb for writing in a way that
 * bypasses the C subagent's single-writer role in the test scenarios that
 * matter for that invariant -- callers choose which environment/flags to
 * open, and the scenario scripts are responsible for using this against
 * cache.lmdb (or a throwaway scratch LMDB dir) when demonstrating
 * multi-writer contention, never against a live config.lmdb that the
 * subagent also has open.
 *
 * usage:
 *   storage_cli open-flags: pass one of "persistent" | "nosync" | "rdonly"
 *   storage_cli get      <path> <flags> int|uint|u64|bytes <key>
 *   storage_cli set      <path> <flags> int|uint|u64|bytes <key> <value>
 *   storage_cli hexdump  <path> <flags> <key>
 *   storage_cli watch    <path> <flags> <key> <iterations> <delay_us>
 *   storage_cli bench    <path> <flags> int|uint|u64|bytes <key> <count>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>

#include "storage_lmdb.h"

static unsigned
parse_flags(const char *s)
{
    if (strcmp(s, "persistent") == 0) return STORAGE_ENV_PERSISTENT;
    if (strcmp(s, "nosync") == 0)     return STORAGE_ENV_NOSYNC;
    if (strcmp(s, "rdonly") == 0)     return STORAGE_ENV_PERSISTENT | STORAGE_ENV_READONLY;
    if (strcmp(s, "nosync-rdonly") == 0) return STORAGE_ENV_NOSYNC | STORAGE_ENV_READONLY;
    fprintf(stderr, "storage_cli: unknown flags '%s'\n", s);
    exit(2);
}

static double
now_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static int
cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static storage_rc_t
do_get(storage_env_t *env, const char *type, const char *key, int print)
{
    storage_rc_t rc;
    if (strcmp(type, "int") == 0) {
        int32_t v;
        rc = storage_get_int(env, key, &v);
        if (rc == STORAGE_OK && print) printf("%d\n", v);
    } else if (strcmp(type, "uint") == 0) {
        uint32_t v;
        rc = storage_get_uint(env, key, &v);
        if (rc == STORAGE_OK && print) printf("%u\n", v);
    } else if (strcmp(type, "u64") == 0) {
        uint64_t v;
        rc = storage_get_u64(env, key, &v);
        if (rc == STORAGE_OK && print) printf("%" PRIu64 "\n", v);
    } else if (strcmp(type, "bytes") == 0) {
        char buf[512];
        size_t len = sizeof(buf) - 1;
        rc = storage_get_bytes(env, key, buf, &len);
        if (rc == STORAGE_OK) {
            buf[len] = '\0';
            if (print) printf("%s\n", buf);
        }
    } else {
        fprintf(stderr, "storage_cli: unknown type '%s'\n", type);
        exit(2);
    }
    return rc;
}

static int
cmd_get(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: get <path> <flags> <type> <key>\n"); return 2; }
    storage_env_t *env;
    storage_rc_t rc = storage_env_open(argv[1], 0, parse_flags(argv[2]), &env);
    if (rc != STORAGE_OK) { fprintf(stderr, "open failed: %s\n", storage_strerror(rc)); return 1; }
    rc = do_get(env, argv[3], argv[4], 1);
    storage_env_close(env);
    if (rc != STORAGE_OK) { fprintf(stderr, "get failed: %s\n", storage_strerror(rc)); return 1; }
    return 0;
}

static int
cmd_set(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: set <path> <flags> <type> <key> <value>\n"); return 2; }
    storage_env_t *env;
    storage_rc_t rc = storage_env_open(argv[1], 0, parse_flags(argv[2]), &env);
    if (rc != STORAGE_OK) { fprintf(stderr, "open failed: %s\n", storage_strerror(rc)); return 1; }

    const char *type = argv[3], *key = argv[4], *value = argv[5];
    if (strcmp(type, "int") == 0) {
        rc = storage_set_int(env, key, (int32_t)strtol(value, NULL, 10));
    } else if (strcmp(type, "uint") == 0) {
        rc = storage_set_uint(env, key, (uint32_t)strtoul(value, NULL, 10));
    } else if (strcmp(type, "u64") == 0) {
        rc = storage_set_u64(env, key, (uint64_t)strtoull(value, NULL, 10));
    } else if (strcmp(type, "bytes") == 0) {
        rc = storage_set_bytes(env, key, value, strlen(value));
    } else {
        fprintf(stderr, "storage_cli: unknown type '%s'\n", type);
        storage_env_close(env);
        return 2;
    }
    storage_env_close(env);
    if (rc != STORAGE_OK) { fprintf(stderr, "set failed: %s\n", storage_strerror(rc)); return 1; }
    printf("OK\n");
    return 0;
}

static int
cmd_hexdump(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: hexdump <path> <flags> <key>\n"); return 2; }
    storage_env_t *env;
    storage_rc_t rc = storage_env_open(argv[1], 0, parse_flags(argv[2]), &env);
    if (rc != STORAGE_OK) { fprintf(stderr, "open failed: %s\n", storage_strerror(rc)); return 1; }

    storage_type_t ty;
    rc = storage_type_of(env, argv[3], &ty);
    if (rc != STORAGE_OK) { fprintf(stderr, "type_of failed: %s\n", storage_strerror(rc)); storage_env_close(env); return 1; }

    unsigned char buf[STORAGE_VALUE_HEADER_LEN + 512];
    size_t len = sizeof(buf) - STORAGE_VALUE_HEADER_LEN;
    /* Reconstruct the full on-disk bytes (header + payload) generically by
     * re-reading via the raw bytes accessor is not exposed, so rebuild from
     * the typed getters -- sufficient for the fixed-width types the demo
     * MIB uses, and bytes/oid via storage_get_bytes/storage_get_oid. */
    buf[0] = (unsigned char)ty;
    buf[1] = buf[2] = buf[3] = 0;
    size_t payload_len = 0;
    switch (ty) {
    case STORAGE_TYPE_INT32: {
        int32_t v; rc = storage_get_int(env, argv[3], &v);
        memcpy(buf + 4, &v, 4); payload_len = 4;
        break;
    }
    case STORAGE_TYPE_UINT32: {
        uint32_t v; rc = storage_get_uint(env, argv[3], &v);
        memcpy(buf + 4, &v, 4); payload_len = 4;
        break;
    }
    case STORAGE_TYPE_UINT64: {
        uint64_t v; rc = storage_get_u64(env, argv[3], &v);
        memcpy(buf + 4, &v, 8); payload_len = 8;
        break;
    }
    case STORAGE_TYPE_BYTES: {
        rc = storage_get_bytes(env, argv[3], buf + 4, &len);
        payload_len = len;
        break;
    }
    default:
        fprintf(stderr, "hexdump: unsupported type tag %d\n", (int)ty);
        storage_env_close(env);
        return 1;
    }
    storage_env_close(env);
    if (rc != STORAGE_OK) { fprintf(stderr, "get failed: %s\n", storage_strerror(rc)); return 1; }

    for (size_t i = 0; i < STORAGE_VALUE_HEADER_LEN + payload_len; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");
    return 0;
}

static int
cmd_watch(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: watch <path> <flags> <key> <iterations> <delay_us>\n"); return 2; }
    storage_env_t *env;
    storage_rc_t rc = storage_env_open(argv[1], 0, parse_flags(argv[2]), &env);
    if (rc != STORAGE_OK) { fprintf(stderr, "open failed: %s\n", storage_strerror(rc)); return 1; }

    long iterations = strtol(argv[4], NULL, 10);
    long delay_us = strtol(argv[5], NULL, 10);
    long errors = 0, notfound = 0, ok = 0;

    for (long i = 0; i < iterations; i++) {
        storage_type_t ty;
        rc = storage_type_of(env, argv[3], &ty);
        if (rc == STORAGE_ERR_NOTFOUND) {
            notfound++;
        } else if (rc != STORAGE_OK) {
            errors++;
            fprintf(stderr, "watch: iter %ld type_of error: %s\n", i, storage_strerror(rc));
        } else {
            /* Decode fully via do_get to exercise the same path a real
             * reader would use; any malformed/torn value surfaces here as
             * STORAGE_ERR_INVAL or STORAGE_ERR_TYPE. */
            const char *tyname = ty == STORAGE_TYPE_INT32 ? "int"
                                : ty == STORAGE_TYPE_UINT32 ? "uint"
                                : ty == STORAGE_TYPE_UINT64 ? "u64"
                                : ty == STORAGE_TYPE_BYTES ? "bytes" : NULL;
            if (tyname == NULL) {
                errors++;
                fprintf(stderr, "watch: iter %ld unsupported tag %d\n", i, (int)ty);
            } else {
                storage_rc_t grc = do_get(env, tyname, argv[3], 0);
                if (grc == STORAGE_OK) ok++;
                else { errors++; fprintf(stderr, "watch: iter %ld decode error: %s\n", i, storage_strerror(grc)); }
            }
        }
        if (delay_us > 0) usleep((useconds_t)delay_us);
    }
    storage_env_close(env);
    printf("ok=%ld notfound=%ld errors=%ld\n", ok, notfound, errors);
    return errors > 0 ? 1 : 0;
}

static int
cmd_bench(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: bench <path> <flags> <type> <key> <count>\n"); return 2; }
    storage_env_t *env;
    storage_rc_t rc = storage_env_open(argv[1], 0, parse_flags(argv[2]), &env);
    if (rc != STORAGE_OK) { fprintf(stderr, "open failed: %s\n", storage_strerror(rc)); return 1; }

    long count = strtol(argv[5], NULL, 10);
    double *samples = calloc((size_t)count, sizeof(double));
    if (!samples) { fprintf(stderr, "bench: OOM\n"); storage_env_close(env); return 1; }

    long errors = 0;
    double t0 = now_monotonic_us();
    for (long i = 0; i < count; i++) {
        double a = now_monotonic_us();
        storage_rc_t grc = do_get(env, argv[3], argv[4], 0);
        double b = now_monotonic_us();
        samples[i] = b - a;
        if (grc != STORAGE_OK && grc != STORAGE_ERR_NOTFOUND) errors++;
    }
    double t1 = now_monotonic_us();
    storage_env_close(env);

    qsort(samples, (size_t)count, sizeof(double), cmp_double);
    double p50 = samples[(size_t)(count * 50 / 100)];
    double p95 = samples[(size_t)(count * 95 / 100 < count ? count * 95 / 100 : count - 1)];
    double p99 = samples[(size_t)(count * 99 / 100 < count ? count * 99 / 100 : count - 1)];
    double total_s = (t1 - t0) / 1e6;
    double throughput = total_s > 0 ? (double)count / total_s : 0.0;

    printf("count=%ld errors=%ld total_s=%.6f throughput_ops=%.1f p50_us=%.2f p95_us=%.2f p99_us=%.2f\n",
           count, errors, total_s, throughput, p50, p95, p99);
    free(samples);
    return errors > 0 ? 1 : 0;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <get|set|hexdump|watch|bench> ...\n"
        "  flags: persistent | nosync | rdonly | nosync-rdonly\n"
        "  get     <path> <flags> <int|uint|u64|bytes> <key>\n"
        "  set     <path> <flags> <int|uint|u64|bytes> <key> <value>\n"
        "  hexdump <path> <flags> <key>\n"
        "  watch   <path> <flags> <key> <iterations> <delay_us>\n"
        "  bench   <path> <flags> <int|uint|u64|bytes> <key> <count>\n",
        argv0);
}

int
main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *cmd = argv[1];
    int rest_argc = argc - 1;
    char **rest_argv = argv + 1;

    if (strcmp(cmd, "get") == 0)      return cmd_get(rest_argc, rest_argv);
    if (strcmp(cmd, "set") == 0)      return cmd_set(rest_argc, rest_argv);
    if (strcmp(cmd, "hexdump") == 0)  return cmd_hexdump(rest_argc, rest_argv);
    if (strcmp(cmd, "watch") == 0)    return cmd_watch(rest_argc, rest_argv);
    if (strcmp(cmd, "bench") == 0)    return cmd_bench(rest_argc, rest_argv);

    usage(argv[0]);
    return 2;
}
