/*
 * storage_mode.h — per-MIB-object storage backend selection.
 *
 * docs/design.md ch.3.1:
 *   read-write objects  : both a config.lmdb and a cache.lmdb implementation
 *                         are generated; the switch below picks one at
 *                         compile time.
 *   read-only  objects  : cache.lmdb only, no switch is emitted.
 *
 * Changing a value here and rebuilding is the only supported way to move an
 * object between the persistent and the volatile environment.
 */
#ifndef STORAGE_MODE_H
#define STORAGE_MODE_H

#define STORAGE_MODE_PERSISTENT 1
#define STORAGE_MODE_VOLATILE   0

/* --- AGENTX-DEMO-MIB read-write scalars ------------------------------ */
#define STORAGE_MODE_deviceName           STORAGE_MODE_PERSISTENT
#define STORAGE_MODE_tempThresholdMilliC  STORAGE_MODE_VOLATILE
#define STORAGE_MODE_adminStatusExt       STORAGE_MODE_PERSISTENT
#define STORAGE_MODE_sampleIntervalSec    STORAGE_MODE_PERSISTENT

/* --- AGENTX-DEMO-MIB read-write table columns ------------------------ */
/*
 * The selection is per COLUMN, not per table (docs/design.md 3.2): the whole
 * row does not have to live in one environment. portConfigTable below is
 * deliberately split -- its two descriptive columns are operator
 * configuration that must survive a reboot, while the alarm threshold is
 * declared volatile and comes back at its DEFVAL, exactly as the
 * tempThresholdMilliC scalar does.
 *
 * How many rows exist is a separate question from where their cells live;
 * see include/table_provision.h.
 */
#define STORAGE_MODE_portDescr                 STORAGE_MODE_PERSISTENT
#define STORAGE_MODE_portAdminStatus           STORAGE_MODE_PERSISTENT
#define STORAGE_MODE_portAlarmThresholdMilliC  STORAGE_MODE_VOLATILE

#endif /* STORAGE_MODE_H */
