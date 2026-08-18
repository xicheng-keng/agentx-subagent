/*
 * demo_trap.c -- AGENTX-DEMO-MIB notification emission (demoTempAlarm).
 *
 * See include/demo_trap.h for the contract. Storage backend selection for
 * tempThresholdMilliC follows the same #if STORAGE_MODE_... pattern used by
 * the generated MIB handlers (src/generated/demo_config.c), driven by
 * storage_mode.h (docs/design.md ch.3.1). cpuTempMilliC is read-only and
 * always lives in cache.lmdb (docs/design.md ch.2.5).
 */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include "demo_trap.h"
#include "storage_lmdb.h"
#include "storage_mode.h"
#include "subagent_env.h"

#ifndef STORAGE_MODE_tempThresholdMilliC
#error "STORAGE_MODE_tempThresholdMilliC is not defined in storage_mode.h -- add an entry for 'tempThresholdMilliC' (see docs/design.md 3.1)"
#endif

/* snmpTrapOID.0 */
static const oid snmp_trap_oid[]        = { 1, 3, 6, 1, 6, 3, 1, 1, 4, 1, 0 };
/* demoTempAlarm notification OID */
static const oid demo_temp_alarm_oid[]  = { 1, 3, 6, 1, 4, 1, 99999, 0, 1 };
/* cpuTempMilliC.0 */
static const oid cpu_temp_oid[]         = { 1, 3, 6, 1, 4, 1, 99999, 2, 1, 0 };
/* tempThresholdMilliC.0 */
static const oid temp_threshold_oid[]   = { 1, 3, 6, 1, 4, 1, 99999, 1, 2, 0 };

/*
 * Edge-detection state for demo_check_temp_alarm(). Starts CLEAR; the first
 * sample above threshold moves to ALARM and fires exactly one trap. We stay
 * in ALARM (no repeated traps) until the temperature drops back below the
 * threshold minus a small hysteresis band, at which point we return to
 * CLEAR so a later crossing traps again. The hysteresis avoids chattering
 * between the two states when cpuTempMilliC oscillates by a few milli-
 * degrees right at the threshold.
 */
typedef enum { TEMP_STATE_CLEAR = 0, TEMP_STATE_ALARM = 1 } temp_state_t;

static temp_state_t temp_alarm_state = TEMP_STATE_CLEAR;

/* 500 milli-degrees C of hysteresis on the way back down to CLEAR. */
#define TEMP_ALARM_HYSTERESIS_MILLIC 500

static storage_rc_t
read_temp_threshold(int32_t *out)
{
#if STORAGE_MODE_tempThresholdMilliC == STORAGE_MODE_PERSISTENT
    storage_env_t *env = config_env;
#else
    storage_env_t *env = cache_env;
#endif
    return storage_get_int(env, "tempThresholdMilliC", out);
}

static storage_rc_t
read_cpu_temp(int32_t *out)
{
    return storage_get_int(cache_env, "cpuTempMilliC", out);
}

int
demo_send_temp_alarm(void)
{
    int32_t cpu_temp;
    int32_t threshold;
    storage_rc_t rc;
    netsnmp_variable_list *var_list = NULL;

    rc = read_cpu_temp(&cpu_temp);
    if (rc == STORAGE_ERR_NOTFOUND) {
        return 0;
    }
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "demo_send_temp_alarm: cpuTempMilliC: %s\n",
                 storage_strerror(rc));
        return -1;
    }

    rc = read_temp_threshold(&threshold);
    if (rc == STORAGE_ERR_NOTFOUND) {
        return 0;
    }
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "demo_send_temp_alarm: tempThresholdMilliC: %s\n",
                 storage_strerror(rc));
        return -1;
    }

    snmp_varlist_add_variable(&var_list,
                               snmp_trap_oid, OID_LENGTH(snmp_trap_oid),
                               ASN_OBJECT_ID,
                               (const u_char *)demo_temp_alarm_oid,
                               sizeof(demo_temp_alarm_oid));
    if (var_list == NULL) {
        snmp_log(LOG_ERR, "demo_send_temp_alarm: snmp_varlist_add_variable failed (snmpTrapOID)\n");
        return -1;
    }

    snmp_varlist_add_variable(&var_list,
                               cpu_temp_oid, OID_LENGTH(cpu_temp_oid),
                               ASN_INTEGER,
                               (const u_char *)&cpu_temp, sizeof(cpu_temp));

    snmp_varlist_add_variable(&var_list,
                               temp_threshold_oid, OID_LENGTH(temp_threshold_oid),
                               ASN_INTEGER,
                               (const u_char *)&threshold, sizeof(threshold));

    send_v2trap(var_list);
    snmp_free_varbind(var_list);

    return 0;
}

int
demo_check_temp_alarm(void)
{
    int32_t cpu_temp;
    int32_t threshold;
    storage_rc_t rc;

    rc = read_cpu_temp(&cpu_temp);
    if (rc == STORAGE_ERR_NOTFOUND) {
        return 0;
    }
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "demo_check_temp_alarm: cpuTempMilliC: %s\n",
                 storage_strerror(rc));
        return -1;
    }

    rc = read_temp_threshold(&threshold);
    if (rc == STORAGE_ERR_NOTFOUND) {
        return 0;
    }
    if (rc != STORAGE_OK) {
        snmp_log(LOG_ERR, "demo_check_temp_alarm: tempThresholdMilliC: %s\n",
                 storage_strerror(rc));
        return -1;
    }

    if (temp_alarm_state == TEMP_STATE_CLEAR) {
        if (cpu_temp > threshold) {
            temp_alarm_state = TEMP_STATE_ALARM;
            if (demo_send_temp_alarm() != 0) {
                return -1;
            }
            return 1;
        }
        return 0;
    }

    /* TEMP_STATE_ALARM: only drop back to CLEAR once we are comfortably
     * below the threshold again, so we don't re-fire on noise right at the
     * boundary. */
    if (cpu_temp <= threshold - TEMP_ALARM_HYSTERESIS_MILLIC) {
        temp_alarm_state = TEMP_STATE_CLEAR;
    }
    return 0;
}
