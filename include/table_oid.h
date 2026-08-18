/*
 * table_oid.h — net-snmp OID <-> table_rows instance conversions.
 *
 * The two directions the generated table handlers need, kept out of the
 * template so there is one implementation rather than one per table:
 *
 *   row instance -> typed index varbinds, for the iterator's
 *                   get_next_data_point() (and, in passing, validation that
 *                   a key's instance matches the table's index syntax)
 *   index varbinds -> row instance, for building the cell key of the row a
 *                   request landed on
 *
 * The second direction exists because a matched row carries no allocated
 * context (see the sentinel in the generated code and docs/design.md 3.2):
 * net-snmp's table iterator hands the same data context to every request
 * that matches the same row and then releases it once per request, so a
 * per-row allocation would be a double free as soon as a manager set two
 * columns of one row in a single PDU. The instance is therefore recovered
 * from the request instead, which also survives the SET phases -- only
 * MODE_SET_RESERVE1 walks the rows, and everything after it works purely
 * from what the request carries.
 */
#ifndef TABLE_OID_H
#define TABLE_OID_H

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include "table_rows.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fill `put_index_data` -- the iterator's index varbind list, typed from the
 * MIB -- from `inst`. Returns 0 on success, -1 if the instance does not
 * decode into exactly this table's indexes, which is how a key that is not a
 * row of this table gets rejected.
 */
int table_instance_to_indexes(const table_instance_t *inst,
                              netsnmp_variable_list *put_index_data);

/*
 * Recover the row instance a request landed on from its parsed index
 * varbinds. Returns 0 on success, -1 if the indexes do not form an instance
 * this layer can represent (too many sub-identifiers, or one that does not
 * fit in 32 bits).
 */
int table_instance_from_indexes(const netsnmp_table_request_info *table_info,
                                table_instance_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TABLE_OID_H */
