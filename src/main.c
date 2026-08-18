/*
 * main.c -- agentx-subagent entry point.
 *
 * Wires together:
 *   - the two LMDB environments (config.lmdb / cache.lmdb, subagent_env.h)
 *   - the generated AGENTX-DEMO-MIB scalar handlers (demo_config.h/demo_status.h)
 *   - the generated AGENTX-DEMO-MIB table handlers
 *     (demo_config_table.h/demo_status_table.h)
 *   - the AF_UNIX/protobuf IPC server (ipc_server.h)
 *   - a 5 second alarm that checks for a temperature-alarm edge (demo_trap.h)
 *
 * into a single net-snmp AgentX subagent event loop. See docs/design.md
 * ch.1, 2.3, 2.5 and 4 for the design rationale.
 */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "subagent_env.h"
#include "storage_lmdb.h"
#include "storage_bootstrap.h"
#include "demo_config.h"
#include "demo_config_table.h"
#include "demo_status.h"
#include "demo_status_table.h"
#include "ipc_server.h"
#include "demo_trap.h"

/* --------------------------------------------------------------------- */
/* process-wide LMDB environment handles (extern in subagent_env.h)      */
/* --------------------------------------------------------------------- */

storage_env_t *config_env = NULL;
storage_env_t *cache_env  = NULL;

/* --------------------------------------------------------------------- */
/* storage init / shutdown (subagent_env.h contract)                     */
/* --------------------------------------------------------------------- */

int
subagent_storage_init(const char *config_path, const char *cache_path)
{
    storage_rc_t rc;
    uint64_t entries_before = 0, entries_after = 0;
    uint64_t bytes_unused;

    if (config_path == NULL || config_path[0] == '\0') {
        config_path = getenv("AGENTX_CONFIG_DB");
        if (config_path == NULL || config_path[0] == '\0') {
            config_path = SUBAGENT_CONFIG_DB_DEFAULT;
        }
    }
    if (cache_path == NULL || cache_path[0] == '\0') {
        cache_path = getenv("AGENTX_CACHE_DB");
        if (cache_path == NULL || cache_path[0] == '\0') {
            cache_path = SUBAGENT_CACHE_DB_DEFAULT;
        }
    }

    rc = storage_env_open(config_path, 0, STORAGE_ENV_PERSISTENT, &config_env);
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "subagent_storage_init: failed to open config env '%s': %s\n",
                  config_path, storage_strerror(rc));
        config_env = NULL;
        return -1;
    }

    rc = config_bootstrap_defaults(config_env);
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "subagent_storage_init: config_bootstrap_defaults failed: %s\n",
                  storage_strerror(rc));
        storage_env_close(config_env);
        config_env = NULL;
        return -1;
    }
    snmp_log(LOG_INFO,
              "subagent_storage_init: config.lmdb bootstrap complete (config='%s')\n",
              config_path);

    rc = storage_env_open(cache_path, 0, STORAGE_ENV_NOSYNC, &cache_env);
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "subagent_storage_init: failed to open cache env '%s': %s\n",
                  cache_path, storage_strerror(rc));
        storage_env_close(config_env);
        config_env = NULL;
        cache_env = NULL;
        return -1;
    }

    /* Best effort stat before/after the bootstrap so we can log how many
     * defaults were actually seeded; a stat failure here is not fatal to
     * startup. */
    (void)storage_env_stat(cache_env, &entries_before, &bytes_unused);

    rc = cache_bootstrap_defaults(cache_env, 0 /* do not overwrite live data */);
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "subagent_storage_init: cache_bootstrap_defaults failed: %s\n",
                  storage_strerror(rc));
        storage_env_close(cache_env);
        storage_env_close(config_env);
        cache_env = NULL;
        config_env = NULL;
        return -1;
    }

    if (storage_env_stat(cache_env, &entries_after, &bytes_unused) == STORAGE_OK) {
        uint64_t seeded = (entries_after > entries_before)
                              ? (entries_after - entries_before) : 0;
        snmp_log(LOG_INFO,
                  "subagent_storage_init: cache.lmdb bootstrap seeded %llu key(s) "
                  "(config='%s' cache='%s')\n",
                  (unsigned long long)seeded, config_path, cache_path);
    } else {
        snmp_log(LOG_INFO,
                  "subagent_storage_init: cache.lmdb bootstrap complete "
                  "(config='%s' cache='%s')\n", config_path, cache_path);
    }

    return 0;
}

void
subagent_storage_shutdown(void)
{
    if (cache_env != NULL) {
        storage_env_close(cache_env);
        cache_env = NULL;
    }
    if (config_env != NULL) {
        storage_env_close(config_env);
        config_env = NULL;
    }
}

/* --------------------------------------------------------------------- */
/* signal handling                                                       */
/* --------------------------------------------------------------------- */

static volatile sig_atomic_t g_stop_requested = 0;

static void
handle_stop_signal(int signum)
{
    (void)signum;
    g_stop_requested = 1;
}

static int
install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: select() must return EINTR */

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------- */
/* alarm callback: notice a temperature crossing without being asked     */
/* --------------------------------------------------------------------- */

static void
temp_alarm_alarm_cb(unsigned int clientreg, void *clientarg)
{
    (void)clientreg;
    (void)clientarg;
    (void)demo_check_temp_alarm();
}

/* --------------------------------------------------------------------- */
/* CLI                                                                   */
/* --------------------------------------------------------------------- */

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [-f] [-L o|f FILE] [-x TRANSPORT] [-C config.lmdb]\n"
        "           [-c cache.lmdb] [-s ipc.sock] [-h]\n"
        "  -f              run in the foreground (do not daemonise)\n"
        "  -L o|f FILE     net-snmp logging: 'o' for stderr, 'f' for a log file\n"
        "  -x TRANSPORT    AgentX master socket (default: net-snmp default)\n"
        "  -C PATH         config.lmdb path (default: %s, or $AGENTX_CONFIG_DB)\n"
        "  -c PATH         cache.lmdb path (default: %s, or $AGENTX_CACHE_DB)\n"
        "  -s PATH         IPC socket path (default: %s, or $AGENTX_IPC_SOCKET)\n"
        "  -h              show this help\n",
        argv0, SUBAGENT_CONFIG_DB_DEFAULT, SUBAGENT_CACHE_DB_DEFAULT,
        IPC_SOCKET_PATH_DEFAULT);
}

int
main(int argc, char *argv[])
{
    int foreground = 0;
    const char *agentx_socket = NULL;
    const char *config_path = NULL;
    const char *cache_path = NULL;
    const char *ipc_path = NULL;
    const char *log_file = NULL;
    int log_mode = 0; /* 0 = default (syslog), 'o' = stderr, 'f' = file */
    int opt;
    ipc_server_t *ipc = NULL;
    int exit_code = 0;

    while ((opt = getopt(argc, argv, "fL:x:C:c:s:h")) != -1) {
        switch (opt) {
        case 'f':
            foreground = 1;
            break;
        case 'L':
            if (strcmp(optarg, "o") == 0) {
                log_mode = 'o';
            } else if (strcmp(optarg, "f") == 0) {
                log_mode = 'f';
                if (optind >= argc) {
                    fprintf(stderr, "%s: -L f requires a FILE argument\n", argv[0]);
                    usage(argv[0]);
                    return 1;
                }
                log_file = argv[optind++];
            } else {
                fprintf(stderr, "%s: -L expects 'o' or 'f'\n", argv[0]);
                usage(argv[0]);
                return 1;
            }
            break;
        case 'x':
            agentx_socket = optarg;
            break;
        case 'C':
            config_path = optarg;
            break;
        case 'c':
            cache_path = optarg;
            break;
        case 's':
            ipc_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[optind]);
        usage(argv[0]);
        return 1;
    }

    if (ipc_path == NULL || ipc_path[0] == '\0') {
        ipc_path = getenv("AGENTX_IPC_SOCKET");
        if (ipc_path == NULL || ipc_path[0] == '\0') {
            ipc_path = IPC_SOCKET_PATH_DEFAULT;
        }
    }

    /* net-snmp logging setup, before init_agent() so early messages land
     * in the right place. */
    switch (log_mode) {
    case 'o':
        snmp_enable_stderrlog();
        break;
    case 'f':
        snmp_enable_filelog(log_file, 1 /* append */);
        break;
    default:
        /* Leave net-snmp's default logging (syslog) in place. */
        break;
    }

    if (install_signal_handlers() != 0) {
        snmp_log(LOG_ERR, "main: failed to install signal handlers: %s\n",
                  strerror(errno));
        return 1;
    }

    /* Daemonise (unless -f) after argument parsing but BEFORE opening the
     * LMDB environments: netsnmp_daemonize() forks, and a forked child must
     * not inherit a parent's already-open LMDB environment (an mdb_env_t
     * holds an flock()'d fd and internal locktable mmap that are not safe
     * to share/duplicate across a fork boundary that keeps both processes
     * alive). Doing storage_init only in the surviving child avoids that
     * entirely. */
    if (!foreground) {
        netsnmp_daemonize(1 /* no chdir */, log_mode == 0 ? 0 : 1 /* keep stdio only if not logging elsewhere */);
    }

    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1);
    if (agentx_socket != NULL) {
        netsnmp_ds_set_string(NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_AGENT_X_SOCKET, agentx_socket);
    }

    init_agent("agentx-subagent");

    if (subagent_storage_init(config_path, cache_path) != 0) {
        snmp_log(LOG_ERR, "main: subagent_storage_init failed, exiting\n");
        snmp_shutdown("agentx-subagent");
        return 1;
    }

    init_demo_config();
    init_demo_config_table();
    init_demo_status();
    init_demo_status_table();

    init_snmp("agentx-subagent");

    ipc = ipc_server_start(ipc_path);
    if (ipc == NULL) {
        snmp_log(LOG_ERR, "main: ipc_server_start('%s') failed, exiting\n", ipc_path);
        subagent_storage_shutdown();
        snmp_shutdown("agentx-subagent");
        return 1;
    }

    snmp_alarm_register(5 /* seconds */, SA_REPEAT, temp_alarm_alarm_cb, NULL);

    snmp_log(LOG_INFO, "agentx-subagent: started (ipc socket '%s')\n", ipc_path);

    while (!g_stop_requested) {
        fd_set fdset;
        int numfds = 0;
        struct timeval timeout = { 1, 0 };
        int block = 0;
        int count;

        FD_ZERO(&fdset);

        /*
         * fd-set convention reconciliation:
         *   - snmp_select_info() follows the classic BSD select() convention:
         *     it treats *numfds as "highest fd currently registered, plus
         *     one" and both reads and updates it that way, i.e. on return
         *     *numfds == max_fd + 1 across everything net-snmp added.
         *   - ipc_server_fill_fdset() instead returns the highest fd it
         *     registered (a raw fd value, NOT already +1), per
         *     include/ipc_server.h ("returns the highest fd registered").
         * We therefore pass ipc_server_fill_fdset() the *current* nfds-1
         * convention consistently by tracking the "max fd so far" (not
         * "count") as we go: call snmp_select_info() first to seed fdset
         * and get its max_fd+1, convert to a plain max-fd value, hand that
         * to ipc_server_fill_fdset() (which unions in the IPC fds and
         * returns the new overall highest fd), then convert back to the
         * select()-style "nfds = max_fd + 1" once, right before calling
         * select().
         */
        snmp_select_info(&numfds, &fdset, &timeout, &block);
        {
            int max_fd = numfds - 1; /* net-snmp's max fd so far (may be -1) */

            max_fd = ipc_server_fill_fdset(ipc, &fdset, max_fd);
            numfds = max_fd + 1;
        }

        count = select(numfds, &fdset, NULL, NULL, block ? NULL : &timeout);

        if (count > 0) {
            snmp_read(&fdset);
            ipc_server_dispatch(ipc, &fdset);
        } else if (count == 0) {
            snmp_timeout();
        } else {
            if (errno == EINTR) {
                continue;
            }
            snmp_log(LOG_ERR, "main: select() failed: %s\n", strerror(errno));
            break;
        }

        run_alarms();
    }

    snmp_log(LOG_INFO, "agentx-subagent: shutting down\n");

    snmp_shutdown("agentx-subagent");
    ipc_server_stop(ipc);
    subagent_storage_shutdown();

    return exit_code;
}
