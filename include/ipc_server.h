/*
 * ipc_server.h — AF_UNIX + protobuf (nanopb) request server, docs/design.md ch.4.
 *
 * The server is integrated into the net-snmp event loop: it exposes its
 * listening and connection descriptors so main.c can add them to the fd_set
 * handed to snmp_select_info()/select().  No threads are used, which keeps the
 * "config.lmdb has exactly one writer" invariant trivially true.
 */
#ifndef IPC_SERVER_H
#define IPC_SERVER_H

#include <sys/select.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_SOCKET_PATH_DEFAULT "/run/agentx-subagent/ipc.sock"
#define IPC_MAX_FRAME_LEN       65536u
#define IPC_MAX_CLIENTS         8

typedef struct ipc_server ipc_server_t;

/* Bind and listen. Any stale socket file at `path` is unlinked first. */
ipc_server_t *ipc_server_start(const char *path);
void          ipc_server_stop(ipc_server_t *srv);

/* Register listening + client fds; returns the highest fd registered. */
int  ipc_server_fill_fdset(ipc_server_t *srv, fd_set *readfds, int maxfd);
/* Accept new clients and process every complete frame that is readable. */
void ipc_server_dispatch(ipc_server_t *srv, fd_set *readfds);

#ifdef __cplusplus
}
#endif
#endif /* IPC_SERVER_H */
