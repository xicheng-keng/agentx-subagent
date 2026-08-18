/*
 * demo_trap.h — AGENTX-DEMO-MIB notification emission.
 */
#ifndef DEMO_TRAP_H
#define DEMO_TRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Emit demoTempAlarm with the current cpuTempMilliC / tempThresholdMilliC
 * varbinds read from storage. Returns 0 on success, -1 on failure.
 */
int demo_send_temp_alarm(void);

/*
 * Evaluate cpuTempMilliC against tempThresholdMilliC and emit the alarm on a
 * clear->alarm edge only (no repeated traps while the condition persists).
 * Returns 1 if a trap was sent, 0 if not, -1 on error.
 */
int demo_check_temp_alarm(void);

#ifdef __cplusplus
}
#endif
#endif /* DEMO_TRAP_H */
