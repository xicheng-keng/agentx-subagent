#define _DEFAULT_SOURCE
/*
 * test_ipc_framing.c — exercises src/ipc_server.c over a real AF_UNIX socket.
 *
 * This binary defines the externs that ipc_server.c/subagent_env.h declare
 * but does not itself define (config_env, cache_env, demo_send_temp_alarm),
 * so it links standalone against agentx_storage + agentx_ipc without
 * main.c or demo_trap.c.
 *
 * Storage-dependent cases are deliberately the last ones run, per the task
 * instructions, so that everything else can be validated even before
 * storage_lmdb.c existed.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ipc_server.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "agentx_ipc.pb.h"
#include "subagent_env.h"
#include "demo_trap.h"

/* --------------------------------------------------------------------- */
/* externs this test provides                                            */
/* --------------------------------------------------------------------- */

storage_env_t *config_env = NULL;
storage_env_t *cache_env  = NULL;

static int g_trap_calls = 0;
int demo_send_temp_alarm(void)
{
    g_trap_calls++;
    return 0;
}

/* --------------------------------------------------------------------- */
/* test bookkeeping                                                       */
/* --------------------------------------------------------------------- */

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* --------------------------------------------------------------------- */
/* background server thread                                              */
/* --------------------------------------------------------------------- */

static ipc_server_t *g_srv;
static volatile int g_stop;
static pthread_t g_thread;

static void *server_thread_main(void *arg)
{
    (void)arg;
    while (!g_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = ipc_server_fill_fdset(g_srv, &rfds, -1);
        struct timeval tv = { 0, 50 * 1000 };
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (rc > 0) {
            ipc_server_dispatch(g_srv, &rfds);
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------- */
/* socket helpers                                                        */
/* --------------------------------------------------------------------- */

static int connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Returns 1 on full success, 0 on clean EOF before n bytes, -1 on error. */
static int read_exact(int fd, void *buf, size_t n)
{
    size_t off = 0;
    uint8_t *p = buf;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        if (r == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 1;
}

static int write_all(int fd, const void *buf, size_t n)
{
    size_t off = 0;
    const uint8_t *p = buf;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static void put_be32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8)  | (uint32_t)in[3];
}

static int encode_envelope(const agentx_ipc_v1_Envelope *env, uint8_t *out, size_t cap, size_t *out_len)
{
    pb_ostream_t os = pb_ostream_from_buffer(out, cap);
    if (!pb_encode(&os, agentx_ipc_v1_Envelope_fields, env)) {
        return -1;
    }
    *out_len = os.bytes_written;
    return 0;
}

/* Sends one framed envelope in a single write() call. */
static int send_envelope_frame(int fd, const agentx_ipc_v1_Envelope *env)
{
    uint8_t payload[512];
    size_t plen;
    if (encode_envelope(env, payload, sizeof(payload), &plen) != 0) {
        return -1;
    }
    uint8_t frame[4 + sizeof(payload)];
    put_be32(frame, (uint32_t)plen);
    memcpy(frame + 4, payload, plen);
    return write_all(fd, frame, 4 + plen);
}

/* Reads one framed envelope. Returns 1 on success, 0 on clean close, -1 on error. */
static int recv_envelope_frame(int fd, agentx_ipc_v1_Envelope *env)
{
    uint8_t hdr[4];
    int rc = read_exact(fd, hdr, 4);
    if (rc <= 0) {
        return rc;
    }
    uint32_t len = get_be32(hdr);
    if (len == 0 || len > IPC_MAX_FRAME_LEN) {
        return -1;
    }
    uint8_t *payload = malloc(len);
    if (!payload) {
        return -1;
    }
    rc = read_exact(fd, payload, len);
    if (rc <= 0) {
        free(payload);
        return rc;
    }
    pb_istream_t is = pb_istream_from_buffer(payload, len);
    *env = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    bool ok = pb_decode(&is, agentx_ipc_v1_Envelope_fields, env);
    free(payload);
    return ok ? 1 : -1;
}

/* --------------------------------------------------------------------- */
/* protocol-level tests (no storage required)                            */
/* --------------------------------------------------------------------- */

static void test_ping_roundtrip(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 1;
    req.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
    req.body.ping_request.nonce = 424242;

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");

    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.request_id == 1, "request_id not echoed");
        CHECK(resp.which_body == agentx_ipc_v1_Envelope_ping_response_tag, "wrong body");
        CHECK(resp.body.ping_response.nonce == 424242, "nonce not echoed");
    }
    close(fd);
}

static void test_split_frame(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 2;
    req.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
    req.body.ping_request.nonce = 7;

    uint8_t payload[512];
    size_t plen;
    CHECK(encode_envelope(&req, payload, sizeof(payload), &plen) == 0, "encode failed");

    uint8_t frame[4 + sizeof(payload)];
    put_be32(frame, (uint32_t)plen);
    memcpy(frame + 4, payload, plen);
    size_t total = 4 + plen;

    /* Dribble the frame out across several writes with delays so the server
     * must reassemble it from partial reads. */
    size_t sent = 0;
    size_t chunk = 3;
    while (sent < total) {
        size_t n = (total - sent < chunk) ? (total - sent) : chunk;
        CHECK(write_all(fd, frame + sent, n) == 0, "partial write failed");
        sent += n;
        usleep(20 * 1000);
    }

    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.which_body == agentx_ipc_v1_Envelope_ping_response_tag, "wrong body");
        CHECK(resp.body.ping_response.nonce == 7, "nonce mismatch after reassembly");
    }
    close(fd);
}

static void test_two_frames_one_write(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope r1 = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    r1.request_id = 10;
    r1.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
    r1.body.ping_request.nonce = 1;

    agentx_ipc_v1_Envelope r2 = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    r2.request_id = 11;
    r2.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
    r2.body.ping_request.nonce = 2;

    uint8_t p1[512], p2[512];
    size_t l1, l2;
    CHECK(encode_envelope(&r1, p1, sizeof(p1), &l1) == 0, "encode1 failed");
    CHECK(encode_envelope(&r2, p2, sizeof(p2), &l2) == 0, "encode2 failed");

    uint8_t buf[4 + sizeof(p1) + 4 + sizeof(p2)];
    size_t off = 0;
    put_be32(buf + off, (uint32_t)l1); off += 4;
    memcpy(buf + off, p1, l1); off += l1;
    put_be32(buf + off, (uint32_t)l2); off += 4;
    memcpy(buf + off, p2, l2); off += l2;

    CHECK(write_all(fd, buf, off) == 0, "single write of two frames failed");

    agentx_ipc_v1_Envelope resp1, resp2;
    CHECK(recv_envelope_frame(fd, &resp1) == 1, "recv1 failed");
    CHECK(recv_envelope_frame(fd, &resp2) == 1, "recv2 failed");
    CHECK(resp1.request_id == 10 && resp1.body.ping_response.nonce == 1, "resp1 mismatch");
    CHECK(resp2.request_id == 11 && resp2.body.ping_response.nonce == 2, "resp2 mismatch");
    close(fd);
}

static void test_oversized_prefix_closes(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    uint8_t hdr[4];
    put_be32(hdr, IPC_MAX_FRAME_LEN + 1);
    CHECK(write_all(fd, hdr, 4) == 0, "write header failed");

    uint8_t buf[4];
    int rc = read_exact(fd, buf, 1);
    CHECK(rc == 0, "expected clean close for oversized prefix, got rc=%d", rc);
    close(fd);
}

static void test_zero_prefix_closes(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    uint8_t hdr[4] = { 0, 0, 0, 0 };
    CHECK(write_all(fd, hdr, 4) == 0, "write header failed");

    uint8_t buf[4];
    int rc = read_exact(fd, buf, 1);
    CHECK(rc == 0, "expected clean close for zero prefix, got rc=%d", rc);
    close(fd);
}

static void test_garbage_payload_closes(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    /* tag=1 (request_id), wiretype=0 (varint), but the stream ends before
     * the varint value byte -- pb_decode must fail. */
    uint8_t hdr[4];
    put_be32(hdr, 1);
    uint8_t payload[1] = { 0x08 };
    CHECK(write_all(fd, hdr, 4) == 0, "write header failed");
    CHECK(write_all(fd, payload, 1) == 0, "write payload failed");

    uint8_t buf[4];
    int rc = read_exact(fd, buf, 1);
    CHECK(rc == 0, "expected clean close for undecodable payload, got rc=%d", rc);
    close(fd);
}

static void test_mid_frame_disconnect_frees_slots(const char *path)
{
    /* Repeat well past IPC_MAX_CLIENTS to catch a slot leak. */
    for (int i = 0; i < IPC_MAX_CLIENTS * 3; i++) {
        int fd = connect_unix(path);
        CHECK(fd >= 0, "connect %d failed", i);
        if (fd < 0) continue;

        uint8_t hdr[4];
        put_be32(hdr, 100); /* promise 100 bytes of payload */
        write_all(fd, hdr, 4);
        uint8_t partial[10] = {0};
        write_all(fd, partial, sizeof(partial)); /* far short of 100 */
        close(fd); /* disconnect mid-frame */
        usleep(10 * 1000); /* give the server thread a chance to notice */
    }

    /* A fresh connection must still work after all that churn. */
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect after churn failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 99;
    req.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
    req.body.ping_request.nonce = 55;
    CHECK(send_envelope_frame(fd, &req) == 0, "send after churn failed");

    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv after churn failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.body.ping_response.nonce == 55, "nonce mismatch after churn");
    }
    close(fd);
}

static void test_more_than_max_clients(const char *path)
{
    int fds[IPC_MAX_CLIENTS + 5];
    int n = (int)(sizeof(fds) / sizeof(fds[0]));
    for (int i = 0; i < n; i++) {
        fds[i] = connect_unix(path);
        CHECK(fds[i] >= 0, "connect %d failed", i);
    }
    usleep(50 * 1000);

    /* The server must not have crashed: a ping on the first connection
     * (guaranteed to have been accepted into a slot before the backlog
     * overflowed) must still work. */
    if (fds[0] >= 0) {
        agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
        req.request_id = 123;
        req.which_body = agentx_ipc_v1_Envelope_ping_request_tag;
        req.body.ping_request.nonce = 321;
        CHECK(send_envelope_frame(fds[0], &req) == 0, "send failed on client 0");
        agentx_ipc_v1_Envelope resp;
        int rc = recv_envelope_frame(fds[0], &resp);
        CHECK(rc == 1, "recv failed on client 0, server may have crashed, rc=%d", rc);
    }

    for (int i = 0; i < n; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

/* --------------------------------------------------------------------- */
/* config validation tests that must not touch storage                   */
/* --------------------------------------------------------------------- */

static void test_unknown_key_invalid(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 20;
    req.which_body = agentx_ipc_v1_Envelope_write_config_request_tag;
    strncpy(req.body.write_config_request.key, "doesNotExist",
            sizeof(req.body.write_config_request.key) - 1);
    req.body.write_config_request.has_value = true;
    req.body.write_config_request.value.type = agentx_ipc_v1_ValueType_VALUE_TYPE_INT32;
    req.body.write_config_request.value.int32_val = 1;

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.which_body == agentx_ipc_v1_Envelope_write_config_response_tag, "wrong body");
        CHECK(resp.body.write_config_response.status == agentx_ipc_v1_Status_STATUS_INVALID,
              "expected STATUS_INVALID for unknown key, got %d",
              (int)resp.body.write_config_response.status);
    }
    close(fd);
}

static void test_out_of_range_invalid(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 21;
    req.which_body = agentx_ipc_v1_Envelope_write_config_request_tag;
    strncpy(req.body.write_config_request.key, "sampleIntervalSec",
            sizeof(req.body.write_config_request.key) - 1);
    req.body.write_config_request.has_value = true;
    req.body.write_config_request.value.type = agentx_ipc_v1_ValueType_VALUE_TYPE_UINT32;
    req.body.write_config_request.value.uint32_val = 999999; /* > 86400 */

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.body.write_config_response.status == agentx_ipc_v1_Status_STATUS_INVALID,
              "expected STATUS_INVALID for out-of-range value, got %d",
              (int)resp.body.write_config_response.status);
    }
    close(fd);
}

static void test_type_mismatch(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 22;
    req.which_body = agentx_ipc_v1_Envelope_write_config_request_tag;
    strncpy(req.body.write_config_request.key, "sampleIntervalSec",
            sizeof(req.body.write_config_request.key) - 1);
    req.body.write_config_request.has_value = true;
    req.body.write_config_request.value.type = agentx_ipc_v1_ValueType_VALUE_TYPE_BYTES;
    req.body.write_config_request.value.bytes_val.size = 3;
    memcpy(req.body.write_config_request.value.bytes_val.bytes, "abc", 3);

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.body.write_config_response.status == agentx_ipc_v1_Status_STATUS_TYPE_MISMATCH,
              "expected STATUS_TYPE_MISMATCH, got %d",
              (int)resp.body.write_config_response.status);
    }
    close(fd);
}

static void test_unexpected_response_body(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 30;
    req.which_body = agentx_ipc_v1_Envelope_write_config_response_tag;
    req.body.write_config_response.status = agentx_ipc_v1_Status_STATUS_OK;

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.which_body == agentx_ipc_v1_Envelope_write_config_response_tag, "wrong body echoed back");
        CHECK(resp.body.write_config_response.status == agentx_ipc_v1_Status_STATUS_INVALID,
              "expected STATUS_INVALID for unexpected response body");
    }
    close(fd);
}

static void test_ping_response_body_closes(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 31;
    req.which_body = agentx_ipc_v1_Envelope_ping_response_tag;
    req.body.ping_response.nonce = 1;

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    uint8_t buf[4];
    int rc = read_exact(fd, buf, 1);
    CHECK(rc == 0, "expected clean close for ping_response body, got rc=%d", rc);
    close(fd);
}

static void test_send_trap(const char *path)
{
    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    int calls_before = g_trap_calls;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 40;
    req.which_body = agentx_ipc_v1_Envelope_send_trap_request_tag;
    strncpy(req.body.send_trap_request.trap_name, "demoTempAlarm",
            sizeof(req.body.send_trap_request.trap_name) - 1);

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.body.send_trap_response.status == agentx_ipc_v1_Status_STATUS_OK,
              "expected STATUS_OK, got %d", (int)resp.body.send_trap_response.status);
    }
    CHECK(g_trap_calls == calls_before + 1, "demo_send_temp_alarm not called exactly once");
    close(fd);

    /* Unknown trap name -> STATUS_INVALID, and no additional call. */
    fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;
    req.request_id = 41;
    strncpy(req.body.send_trap_request.trap_name, "notARealTrap",
            sizeof(req.body.send_trap_request.trap_name) - 1);
    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    rc = recv_envelope_frame(fd, &resp);
    CHECK(rc == 1, "recv failed rc=%d", rc);
    if (rc == 1) {
        CHECK(resp.body.send_trap_response.status == agentx_ipc_v1_Status_STATUS_INVALID,
              "expected STATUS_INVALID for unknown trap name");
    }
    CHECK(g_trap_calls == calls_before + 1, "demo_send_temp_alarm called for unknown trap");
    close(fd);
}

/* --------------------------------------------------------------------- */
/* storage-dependent test (last, per instructions)                       */
/* --------------------------------------------------------------------- */

static void test_valid_write_readable_via_storage(const char *path, const char *config_dir)
{
    storage_rc_t rc = storage_env_open(config_dir, 0, STORAGE_ENV_PERSISTENT, &config_env);
    CHECK(rc == STORAGE_OK, "storage_env_open(config) failed: %s", storage_strerror(rc));
    if (rc != STORAGE_OK) return;

    int fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;

    agentx_ipc_v1_Envelope req = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    req.request_id = 50;
    req.which_body = agentx_ipc_v1_Envelope_write_config_request_tag;
    strncpy(req.body.write_config_request.key, "sampleIntervalSec",
            sizeof(req.body.write_config_request.key) - 1);
    req.body.write_config_request.has_value = true;
    req.body.write_config_request.value.type = agentx_ipc_v1_ValueType_VALUE_TYPE_UINT32;
    req.body.write_config_request.value.uint32_val = 42;

    CHECK(send_envelope_frame(fd, &req) == 0, "send failed");
    agentx_ipc_v1_Envelope resp;
    int rrc = recv_envelope_frame(fd, &resp);
    CHECK(rrc == 1, "recv failed rc=%d", rrc);
    if (rrc == 1) {
        CHECK(resp.body.write_config_response.status == agentx_ipc_v1_Status_STATUS_OK,
              "expected STATUS_OK, got %d (%s)",
              (int)resp.body.write_config_response.status,
              resp.body.write_config_response.message);
    }
    close(fd);

    /* Read it straight back out through the storage API (not through IPC),
     * to prove the write really landed in config.lmdb. */
    uint32_t stored = 0;
    rc = storage_get_uint(config_env, "sampleIntervalSec", &stored);
    CHECK(rc == STORAGE_OK, "storage_get_uint failed: %s", storage_strerror(rc));
    CHECK(stored == 42, "expected 42, got %u", stored);

    /* And confirm it's readable back through the IPC ReadConfigRequest path too. */
    fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;
    agentx_ipc_v1_Envelope rreq = (agentx_ipc_v1_Envelope)agentx_ipc_v1_Envelope_init_zero;
    rreq.request_id = 51;
    rreq.which_body = agentx_ipc_v1_Envelope_read_config_request_tag;
    strncpy(rreq.body.read_config_request.key, "sampleIntervalSec",
            sizeof(rreq.body.read_config_request.key) - 1);
    CHECK(send_envelope_frame(fd, &rreq) == 0, "send failed");
    rrc = recv_envelope_frame(fd, &resp);
    CHECK(rrc == 1, "recv failed rc=%d", rrc);
    if (rrc == 1) {
        CHECK(resp.body.read_config_response.status == agentx_ipc_v1_Status_STATUS_OK,
              "expected STATUS_OK reading back, got %d", (int)resp.body.read_config_response.status);
        CHECK(resp.body.read_config_response.has_value, "expected has_value true");
        CHECK(resp.body.read_config_response.value.uint32_val == 42,
              "expected 42 via IPC read, got %u", resp.body.read_config_response.value.uint32_val);
    }
    close(fd);

    /* Unknown key via ReadConfigRequest -> STATUS_INVALID; missing-but-known
     * key (never written) -> STATUS_NOT_FOUND. */
    fd = connect_unix(path);
    CHECK(fd >= 0, "connect failed");
    if (fd < 0) return;
    rreq.request_id = 52;
    strncpy(rreq.body.read_config_request.key, "adminStatusExt",
            sizeof(rreq.body.read_config_request.key) - 1);
    CHECK(send_envelope_frame(fd, &rreq) == 0, "send failed");
    rrc = recv_envelope_frame(fd, &resp);
    CHECK(rrc == 1, "recv failed rc=%d", rrc);
    if (rrc == 1) {
        CHECK(resp.body.read_config_response.status == agentx_ipc_v1_Status_STATUS_NOT_FOUND,
              "expected STATUS_NOT_FOUND for never-written key, got %d",
              (int)resp.body.read_config_response.status);
    }
    close(fd);
}

/* --------------------------------------------------------------------- */

static char g_sock_path[256];
static char g_config_dir[256];

static void cleanup(void)
{
    g_stop = 1;
    pthread_join(g_thread, NULL);
    ipc_server_stop(g_srv);
    if (config_env) {
        storage_env_close(config_env);
    }
    if (cache_env) {
        storage_env_close(cache_env);
    }
}

int main(void)
{
    char tmpl[] = "/tmp/agentx_ipc_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    snprintf(g_sock_path, sizeof(g_sock_path), "%s/ipc.sock", dir);
    snprintf(g_config_dir, sizeof(g_config_dir), "%s/config.lmdb", dir);
    mkdir(g_config_dir, 0755);

    g_srv = ipc_server_start(g_sock_path);
    if (!g_srv) {
        fprintf(stderr, "ipc_server_start failed\n");
        return 1;
    }

    g_stop = 0;
    if (pthread_create(&g_thread, NULL, server_thread_main, NULL) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        ipc_server_stop(g_srv);
        return 1;
    }

    /* --- protocol-level, no storage required --- */
    test_ping_roundtrip(g_sock_path);
    test_split_frame(g_sock_path);
    test_two_frames_one_write(g_sock_path);
    test_oversized_prefix_closes(g_sock_path);
    test_zero_prefix_closes(g_sock_path);
    test_garbage_payload_closes(g_sock_path);
    test_mid_frame_disconnect_frees_slots(g_sock_path);
    test_more_than_max_clients(g_sock_path);
    test_unknown_key_invalid(g_sock_path);
    test_out_of_range_invalid(g_sock_path);
    test_type_mismatch(g_sock_path);
    test_unexpected_response_body(g_sock_path);
    test_ping_response_body_closes(g_sock_path);
    test_send_trap(g_sock_path);

    /* --- storage-dependent, last --- */
    test_valid_write_readable_via_storage(g_sock_path, g_config_dir);

    cleanup();

    if (g_failures == 0) {
        printf("test_ipc_framing: all tests passed\n");
        return 0;
    }
    printf("test_ipc_framing: %d failure(s)\n", g_failures);
    return 1;
}
