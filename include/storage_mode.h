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

#endif /* STORAGE_MODE_H */
