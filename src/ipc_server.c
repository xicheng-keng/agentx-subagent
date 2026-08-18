/*
 * ipc_server.c — AF_UNIX + protobuf (nanopb) request server.
 *
 * Implements the contract documented in include/ipc_server.h and
 * proto/agentx_ipc.proto (docs/design.md ch.4): a single-threaded,
 * non-blocking AF_UNIX SOCK_STREAM server, framed as
 *
 *     [4-byte big-endian length][that many bytes of a serialised Envelope]
 *
 * The server is driven by the caller's own select() loop via
 * ipc_server_fill_fdset()/ipc_server_dispatch() so it never blocks and never
 * introduces a second config.lmdb writer.
 *
 * Protocol-error policy for a decoded Envelope that carries an unexpected
 * body (see handle_frame() below):
 *   - A body that is itself one of the *_response variants (write/read
 *     config response, send-trap response) implies what a matching answer
 *     would look like -- those messages all carry a `status` + `message`
 *     pair -- so we answer on that same body with STATUS_INVALID rather
 *     than dropping the connection.
 *   - PingResponse carries only a nonce, with no status field to express an
 *     error in, and an Envelope with no body set at all (which_body == 0,
 *     since none of the oneof field tags is 0) gives us no response shape to
 *     imply either. Both of those cases close the connection instead.
 */
#include "ipc_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "pb_encode.h"
#include "pb_decode.h"
#include "agentx_ipc.pb.h"

#include "subagent_env.h"
#include "storage_mode.h"
#include "demo_trap.h"

/* Each client's receive buffer needs to hold at least one maximum sized
 * frame (4 byte header + IPC_MAX_FRAME_LEN payload). We size it to fit two,
 * so that pipelined frames arriving in one read() are never starved for
 * buffer space before we get a chance to drain what is already complete. */
#define IPC_CLIENT_BUF_SIZE ((size_t)(4 + IPC_MAX_FRAME_LEN) * 2)

/* Scratch buffer for outgoing responses. All response messages are small
 * (largest is a ReadConfigResponse with a 256-byte bytes_val plus a 128-byte
 * message), so this is comfortably oversized. */
#define IPC_SEND_BUF_SIZE 1024u

typedef struct {
    int    fd;
    int    in_use;
    size_t len;                        /* valid bytes at the front of buf */
    uint8_t buf[IPC_CLIENT_BUF_SIZE];
} ipc_client_t;

struct ipc_server {
    int          listen_fd;
    char         sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    ipc_client_t clients[IPC_MAX_CLIENTS];
};

/* --------------------------------------------------------------------- */
/* config object allowlist                                               */
/* --------------------------------------------------------------------- */

typedef enum { CFG_ENV_CONFIG, CFG_ENV_CACHE } cfg_env_id_t;

typedef struct {
    const char             *name;
    agentx_ipc_v1_ValueType type;
    cfg_env_id_t            env;
    int64_t                 min;       /* inclusive; length bound for BYTES */
    int64_t                 max;       /* inclusive; length bound for BYTES */
} config_object_t;

/* Same #if pattern the generated MIB handlers use (docs/design.md 3.1) to
 * pick config_env vs cache_env per object, at compile time from
 * storage_mode.h. */
#if STORAGE_MODE_deviceName == STORAGE_MODE_PERSISTENT
#define CFG_ENV_deviceName CFG_ENV_CONFIG
#else
#define CFG_ENV_deviceName CFG_ENV_CACHE
#endif

#if STORAGE_MODE_tempThresholdMilliC == STORAGE_MODE_PERSISTENT
#define CFG_ENV_tempThresholdMilliC CFG_ENV_CONFIG
#else
#define CFG_ENV_tempThresholdMilliC CFG_ENV_CACHE
#endif

#if STORAGE_MODE_adminStatusExt == STORAGE_MODE_PERSISTENT
#define CFG_ENV_adminStatusExt CFG_ENV_CONFIG
#else
#define CFG_ENV_adminStatusExt CFG_ENV_CACHE
#endif

#if STORAGE_MODE_sampleIntervalSec == STORAGE_MODE_PERSISTENT
#define CFG_ENV_sampleIntervalSec CFG_ENV_CONFIG
#else
#define CFG_ENV_sampleIntervalSec CFG_ENV_CACHE
#endif

static const config_object_t g_config_objects[] = {
    { "deviceName",           agentx_ipc_v1_ValueType_VALUE_TYPE_BYTES,
      CFG_ENV_deviceName,           0,      63 },
    { "tempThresholdMilliC",  agentx_ipc_v1_ValueType_VALUE_TYPE_INT32,
      CFG_ENV_tempThresholdMilliC,  -40000, 125000 },
    { "adminStatusExt",       agentx_ipc_v1_ValueType_VALUE_TYPE_INT32,
      CFG_ENV_adminStatusExt,       1,      3 },
    { "sampleIntervalSec",    agentx_ipc_v1_ValueType_VALUE_TYPE_UINT32,
      CFG_ENV_sampleIntervalSec,    1,      86400 },
};
#define NUM_CONFIG_OBJECTS (sizeof(g_config_objects) / sizeof(g_config_objects[0]))

static const config_object_t *find_config_object(const char *key)
{
    for (size_t i = 0; i < NUM_CONFIG_OBJECTS; i++) {
        if (strcmp(g_config_objects[i].name, key) == 0) {
            return &g_config_objects[i];
        }
    }
    return NULL;
}

static storage_env_t *env_for(cfg_env_id_t id)
{
    return (id == CFG_ENV_CONFIG) ? config_env : cache_env;
}

static agentx_ipc_v1_Status storage_rc_to_status(storage_rc_t rc)
{
    switch (rc) {
    case STORAGE_OK:            return agentx_ipc_v1_Status_STATUS_OK;
    case STORAGE_ERR_NOTFOUND:  return agentx_ipc_v1_Status_STATUS_NOT_FOUND;
    case STORAGE_ERR_TYPE:      return agentx_ipc_v1_Status_STATUS_TYPE_MISMATCH;
    case STORAGE_ERR_INVAL:     return agentx_ipc_v1_Status_STATUS_INVALID;
    case STORAGE_ERR_TOOBIG:    return agentx_ipc_v1_Status_STATUS_INVALID;
    case STORAGE_ERR_FULL:      return agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
    case STORAGE_ERR_BUSY:      return agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
    case STORAGE_ERR_IO:        return agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
    case STORAGE_ERR_READONLY:  return agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
    default:                    return agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
    }
}

/* --------------------------------------------------------------------- */
/* request handling                                                       */
/* --------------------------------------------------------------------- */

static void handle_write_config(const agentx_ipc_v1_WriteConfigRequest *req,
                                 agentx_ipc_v1_WriteConfigResponse *resp)
{
    resp->status = agentx_ipc_v1_Status_STATUS_UNSPECIFIED;
    resp->message[0] = '\0';

    const config_object_t *obj = find_config_object(req->key);
    if (!obj) {
        resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp->message, sizeof(resp->message),
                 "unknown config key '%s'", req->key);
        return;
    }
    if (!req->has_value) {
        resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp->message, sizeof(resp->message), "missing value");
        return;
    }
    if (req->value.type != obj->type) {
        resp->status = agentx_ipc_v1_Status_STATUS_TYPE_MISMATCH;
        snprintf(resp->message, sizeof(resp->message),
                 "'%s' expects value type %d, got %d",
                 obj->name, (int)obj->type, (int)req->value.type);
        return;
    }

    storage_rc_t rc;
    switch (obj->type) {
    case agentx_ipc_v1_ValueType_VALUE_TYPE_INT32: {
        int32_t v = req->value.int32_val;
        if (v < obj->min || v > obj->max) {
            resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
            snprintf(resp->message, sizeof(resp->message),
                     "'%s' value %d out of range [%lld, %lld]",
                     obj->name, v, (long long)obj->min, (long long)obj->max);
            return;
        }
        rc = storage_set_int(env_for(obj->env), obj->name, v);
        break;
    }
    case agentx_ipc_v1_ValueType_VALUE_TYPE_UINT32: {
        uint32_t v = req->value.uint32_val;
        if ((int64_t)v < obj->min || (int64_t)v > obj->max) {
            resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
            snprintf(resp->message, sizeof(resp->message),
                     "'%s' value %u out of range [%lld, %lld]",
                     obj->name, v, (long long)obj->min, (long long)obj->max);
            return;
        }
        rc = storage_set_uint(env_for(obj->env), obj->name, v);
        break;
    }
    case agentx_ipc_v1_ValueType_VALUE_TYPE_BYTES: {
        size_t len = req->value.bytes_val.size;
        if ((int64_t)len < obj->min || (int64_t)len > obj->max) {
            resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
            snprintf(resp->message, sizeof(resp->message),
                     "'%s' length %zu out of range [%lld, %lld]",
                     obj->name, len, (long long)obj->min, (long long)obj->max);
            return;
        }
        rc = storage_set_bytes(env_for(obj->env), obj->name,
                                req->value.bytes_val.bytes, len);
        break;
    }
    default:
        resp->status = agentx_ipc_v1_Status_STATUS_TYPE_MISMATCH;
        snprintf(resp->message, sizeof(resp->message),
                 "'%s' has no supported value type", obj->name);
        return;
    }

    resp->status = storage_rc_to_status(rc);
    if (resp->status != agentx_ipc_v1_Status_STATUS_OK) {
        snprintf(resp->message, sizeof(resp->message),
                 "storage error: %s", storage_strerror(rc));
    }
}

static void handle_read_config(const agentx_ipc_v1_ReadConfigRequest *req,
                                agentx_ipc_v1_ReadConfigResponse *resp)
{
    resp->status = agentx_ipc_v1_Status_STATUS_UNSPECIFIED;
    resp->message[0] = '\0';
    resp->has_value = false;

    const config_object_t *obj = find_config_object(req->key);
    if (!obj) {
        resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp->message, sizeof(resp->message),
                 "unknown config key '%s'", req->key);
        return;
    }

    storage_rc_t rc = STORAGE_ERR_INVAL;
    resp->value = (agentx_ipc_v1_Value)agentx_ipc_v1_Value_init_zero;
    resp->value.type = obj->type;

    switch (obj->type) {
    case agentx_ipc_v1_ValueType_VALUE_TYPE_INT32:
        rc = storage_get_int(env_for(obj->env), obj->name, &resp->value.int32_val);
        break;
    case agentx_ipc_v1_ValueType_VALUE_TYPE_UINT32:
        rc = storage_get_uint(env_for(obj->env), obj->name, &resp->value.uint32_val);
        break;
    case agentx_ipc_v1_ValueType_VALUE_TYPE_BYTES: {
        size_t len = sizeof(resp->value.bytes_val.bytes);
        rc = storage_get_bytes(env_for(obj->env), obj->name,
                                resp->value.bytes_val.bytes, &len);
        if (rc == STORAGE_OK) {
            resp->value.bytes_val.size = (pb_size_t)len;
        }
        break;
    }
    default:
        resp->status = agentx_ipc_v1_Status_STATUS_TYPE_MISMATCH;
        return;
    }

    resp->status = storage_rc_to_status(rc);
    if (resp->status == agentx_ipc_v1_Status_STATUS_OK) {
        resp->has_value = true;
    } else {
        snprintf(resp->message, sizeof(resp->message),
                 "storage error: %s", storage_strerror(rc));
    }
}

static void handle_send_trap(const agentx_ipc_v1_SendTrapRequest *req,
                              agentx_ipc_v1_SendTrapResponse *resp)
{
    resp->message[0] = '\0';

    if (strcmp(req->trap_name, "demoTempAlarm") != 0) {
        resp->status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp->message, sizeof(resp->message),
                 "unsupported trap name '%s'", req->trap_name);
        return;
    }

    if (demo_send_temp_alarm() == 0) {
        resp->status = agentx_ipc_v1_Status_STATUS_OK;
    } else {
        resp->status = agentx_ipc_v1_Status_STATUS_STORAGE_ERROR;
        snprintf(resp->message, sizeof(resp->message), "failed to emit demoTempAlarm");
    }
}

/* --------------------------------------------------------------------- */
/* connection plumbing                                                    */
/* --------------------------------------------------------------------- */

static void close_client(ipc_server_t *srv, ipc_client_t *c, const char *reason)
{
    (void)srv;
    if (reason) {
        fprintf(stderr, "ipc_server: closing fd=%d: %s\n", c->fd, reason);
    }
    close(c->fd);
    c->fd = -1;
    c->in_use = 0;
    c->len = 0;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            poll(&pfd, 1, 1000);
            continue;
        }
        return -1;
    }
    return 0;
}

static void send_envelope(ipc_server_t *srv, ipc_client_t *c,
                           const agentx_ipc_v1_Envelope *env)
{
    uint8_t frame[4 + IPC_SEND_BUF_SIZE];
    pb_ostream_t ostream = pb_ostream_from_buffer(frame + 4, sizeof(frame) - 4);
    if (!pb_encode(&ostream, agentx_ipc_v1_Envelope_fields, env)) {
        fprintf(stderr, "ipc_server: pb_encode failed for fd=%d: %s\n",
                c->fd, PB_GET_ERROR(&ostream));
        close_client(srv, c, "encode failure");
        return;
    }
    uint32_t len = (uint32_t)ostream.bytes_written;
    frame[0] = (uint8_t)((len >> 24) & 0xFFu);
    frame[1] = (uint8_t)((len >> 16) & 0xFFu);
    frame[2] = (uint8_t)((len >> 8)  & 0xFFu);
    frame[3] = (uint8_t)(len         & 0xFFu);

    if (send_all(c->fd, frame, 4 + (size_t)len) != 0) {
        close_client(srv, c, "write failure");
    }
}

/* Decode and act on one complete frame. Returns false if the client was
 * closed while handling it (caller must stop touching *c). */
static bool handle_frame(ipc_server_t *srv, ipc_client_t *c,
                          const uint8_t *data, size_t len)
{
    agentx_ipc_v1_Envelope req = agentx_ipc_v1_Envelope_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&istream, agentx_ipc_v1_Envelope_fields, &req)) {
        fprintf(stderr, "ipc_server: pb_decode failed for fd=%d: %s\n",
                c->fd, PB_GET_ERROR(&istream));
        close_client(srv, c, "malformed envelope");
        return false;
    }

    agentx_ipc_v1_Envelope resp = agentx_ipc_v1_Envelope_init_zero;
    resp.request_id = req.request_id;

    switch (req.which_body) {
    case agentx_ipc_v1_Envelope_ping_request_tag:
        resp.which_body = agentx_ipc_v1_Envelope_ping_response_tag;
        resp.body.ping_response.nonce = req.body.ping_request.nonce;
        break;

    case agentx_ipc_v1_Envelope_write_config_request_tag:
        resp.which_body = agentx_ipc_v1_Envelope_write_config_response_tag;
        handle_write_config(&req.body.write_config_request,
                             &resp.body.write_config_response);
        break;

    case agentx_ipc_v1_Envelope_read_config_request_tag:
        resp.which_body = agentx_ipc_v1_Envelope_read_config_response_tag;
        handle_read_config(&req.body.read_config_request,
                            &resp.body.read_config_response);
        break;

    case agentx_ipc_v1_Envelope_send_trap_request_tag:
        resp.which_body = agentx_ipc_v1_Envelope_send_trap_response_tag;
        handle_send_trap(&req.body.send_trap_request,
                          &resp.body.send_trap_response);
        break;

    /* Response-shaped bodies from a client: answer STATUS_INVALID on the
     * same body shape (see file header comment). */
    case agentx_ipc_v1_Envelope_write_config_response_tag:
        resp.which_body = agentx_ipc_v1_Envelope_write_config_response_tag;
        resp.body.write_config_response.status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp.body.write_config_response.message,
                 sizeof(resp.body.write_config_response.message),
                 "unexpected response envelope from client");
        break;

    case agentx_ipc_v1_Envelope_read_config_response_tag:
        resp.which_body = agentx_ipc_v1_Envelope_read_config_response_tag;
        resp.body.read_config_response.status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp.body.read_config_response.message,
                 sizeof(resp.body.read_config_response.message),
                 "unexpected response envelope from client");
        break;

    case agentx_ipc_v1_Envelope_send_trap_response_tag:
        resp.which_body = agentx_ipc_v1_Envelope_send_trap_response_tag;
        resp.body.send_trap_response.status = agentx_ipc_v1_Status_STATUS_INVALID;
        snprintf(resp.body.send_trap_response.message,
                 sizeof(resp.body.send_trap_response.message),
                 "unexpected response envelope from client");
        break;

    /* PingResponse has no status field to carry an error in, and no body at
     * all gives us no response shape to imply -- close the connection. */
    case agentx_ipc_v1_Envelope_ping_response_tag:
    default:
        close_client(srv, c, "protocol error: unexpected/empty envelope body");
        return false;
    }

    send_envelope(srv, c, &resp);
    return c->in_use != 0;
}

/* Process every complete frame currently buffered for c. Returns false if
 * the client was closed while draining (caller must stop touching *c). */
static bool drain_frames(ipc_server_t *srv, ipc_client_t *c)
{
    for (;;) {
        if (c->len < 4) {
            return true; /* header incomplete, wait for more bytes */
        }
        uint32_t frame_len = ((uint32_t)c->buf[0] << 24) |
                             ((uint32_t)c->buf[1] << 16) |
                             ((uint32_t)c->buf[2] << 8)  |
                              (uint32_t)c->buf[3];

        if (frame_len == 0) {
            close_client(srv, c, "zero length frame prefix");
            return false;
        }
        if (frame_len > IPC_MAX_FRAME_LEN) {
            close_client(srv, c, "frame length prefix exceeds IPC_MAX_FRAME_LEN");
            return false;
        }

        size_t total = 4 + (size_t)frame_len;
        if (c->len < total) {
            return true; /* payload incomplete, wait for more bytes */
        }

        if (!handle_frame(srv, c, c->buf + 4, frame_len)) {
            return false; /* client was closed by handle_frame */
        }

        memmove(c->buf, c->buf + total, c->len - total);
        c->len -= total;
    }
}

/* --------------------------------------------------------------------- */
/* public API                                                             */
/* --------------------------------------------------------------------- */

ipc_server_t *ipc_server_start(const char *path)
{
    if (!path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "ipc_server: socket path too long or missing\n");
        return NULL;
    }

    ipc_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        fprintf(stderr, "ipc_server: out of memory\n");
        return NULL;
    }
    srv->listen_fd = -1;
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        srv->clients[i].fd = -1;
    }
    strncpy(srv->sock_path, path, sizeof(srv->sock_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "ipc_server: socket() failed: %s\n", strerror(errno));
        free(srv);
        return NULL;
    }

    /* Remove any stale socket file left behind by a previous run. */
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "ipc_server: unlink(%s) failed: %s\n", path, strerror(errno));
        close(fd);
        free(srv);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "ipc_server: bind(%s) failed: %s\n", path, strerror(errno));
        close(fd);
        free(srv);
        return NULL;
    }

    if (listen(fd, 8) != 0) {
        fprintf(stderr, "ipc_server: listen() failed: %s\n", strerror(errno));
        close(fd);
        unlink(path);
        free(srv);
        return NULL;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        fprintf(stderr, "ipc_server: fcntl(O_NONBLOCK) failed: %s\n", strerror(errno));
        close(fd);
        unlink(path);
        free(srv);
        return NULL;
    }

    if (chmod(path, 0660) != 0) {
        fprintf(stderr, "ipc_server: chmod(%s) failed: %s\n", path, strerror(errno));
        close(fd);
        unlink(path);
        free(srv);
        return NULL;
    }

    srv->listen_fd = fd;
    return srv;
}

void ipc_server_stop(ipc_server_t *srv)
{
    if (!srv) {
        return;
    }
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (srv->clients[i].in_use) {
            close(srv->clients[i].fd);
        }
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        unlink(srv->sock_path);
    }
    free(srv);
}

int ipc_server_fill_fdset(ipc_server_t *srv, fd_set *readfds, int maxfd)
{
    if (!srv || srv->listen_fd < 0) {
        return maxfd;
    }
    FD_SET(srv->listen_fd, readfds);
    if (srv->listen_fd > maxfd) {
        maxfd = srv->listen_fd;
    }
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (srv->clients[i].in_use) {
            FD_SET(srv->clients[i].fd, readfds);
            if (srv->clients[i].fd > maxfd) {
                maxfd = srv->clients[i].fd;
            }
        }
    }
    return maxfd;
}

static int find_free_slot(ipc_server_t *srv)
{
    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        if (!srv->clients[i].in_use) {
            return i;
        }
    }
    return -1;
}

static void accept_new_clients(ipc_server_t *srv)
{
    for (;;) {
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "ipc_server: accept() failed: %s\n", strerror(errno));
            return;
        }

        /* New fds inherit non-blocking mode from nothing in particular on
         * Linux accept(); set it explicitly. */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);

        int slot = find_free_slot(srv);
        if (slot < 0) {
            /* Backlog is full; accept and immediately close so the kernel
             * backlog doesn't wedge for everyone else. */
            close(fd);
            continue;
        }

        ipc_client_t *c = &srv->clients[slot];
        c->fd = fd;
        c->in_use = 1;
        c->len = 0;
    }
}

static void service_client(ipc_server_t *srv, ipc_client_t *c)
{
    for (;;) {
        if (c->len >= sizeof(c->buf)) {
            /* Should not happen: the buffer is sized for two max frames and
             * we always drain complete frames before returning here. Treat
             * it defensively as a protocol error rather than looping. */
            close_client(srv, c, "receive buffer full without a complete frame");
            return;
        }

        ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - c->len);
        if (n > 0) {
            c->len += (size_t)n;
            if (!drain_frames(srv, c)) {
                return; /* client was closed */
            }
            continue; /* there may be more queued in the kernel */
        }
        if (n == 0) {
            close_client(srv, c, NULL); /* orderly disconnect, no log needed */
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* nothing more for now */
        }
        if (errno == EINTR) {
            continue;
        }
        close_client(srv, c, strerror(errno));
        return;
    }
}

void ipc_server_dispatch(ipc_server_t *srv, fd_set *readfds)
{
    if (!srv || srv->listen_fd < 0) {
        return;
    }

    if (FD_ISSET(srv->listen_fd, readfds)) {
        accept_new_clients(srv);
    }

    for (int i = 0; i < IPC_MAX_CLIENTS; i++) {
        ipc_client_t *c = &srv->clients[i];
        if (c->in_use && FD_ISSET(c->fd, readfds)) {
            service_client(srv, c);
        }
    }
}
