/*
 * table_provision.h — how many rows a configuration table ships with.
 *
 * docs/design.md 3.2. A conceptual table's rows are not derivable from the
 * MIB: a DEFVAL on a columnar object says what a cell of a row starts at, not
 * that the row exists. Nothing in this design lets a manager create one
 * either -- there is no RowStatus column, deliberately -- so a configuration
 * table that nobody seeds would be permanently empty, and a manager walking
 * it would get an empty table where the MIB promises configuration. That is
 * the same failure mode as an unseeded persistent scalar (appendix A.2),
 * which is why the answer is the same: seed it at startup.
 *
 * Row count is therefore a product decision, declared here per table exactly
 * as storage_mode.h declares the backend per object, and consumed by the
 * generated bootstrap (src/generated/storage_bootstrap.c). Adding a row is a
 * recompile, and because seeding never overwrites an existing key, growing
 * this number in a firmware update fills in only the new rows and leaves the
 * operator's settings on the existing ones alone.
 *
 * Only tables with at least one read-write column need an entry: read-only
 * telemetry tables are written by the Rust application, whose rows come and
 * go with the hardware it finds, and seeding those would fabricate sensors
 * that do not exist.
 */
#ifndef TABLE_PROVISION_H
#define TABLE_PROVISION_H

/*
 * AGENTX-DEMO-MIB portConfigTable: four ports, indexes 1..4.
 * Must stay within portIndex's SYNTAX range (1..64), since a provisioned
 * instance outside it would be a row the MIB says cannot exist.
 */
#define TABLE_ROWS_portConfigTable 4

#endif /* TABLE_PROVISION_H */
