/*
 * table_oid.c — implementation of include/table_oid.h.
 *
 * Both directions go through net-snmp's own index codec (parse_oid_indexes /
 * build_oid_noalloc) rather than any local knowledge of index syntax, which
 * is what lets the generated handlers serve integer, string and multi-object
 * indexes with the same code.
 */
#include "table_oid.h"

#include <string.h>

int
table_instance_to_indexes(const table_instance_t *inst,
                          netsnmp_variable_list *put_index_data)
{
    oid    subids[TABLE_ROW_SUBID_MAX];
    size_t i;

    if (inst == NULL || put_index_data == NULL) {
        return -1;
    }
    if (inst->len == 0 || inst->len > TABLE_ROW_SUBID_MAX) {
        return -1;
    }

    for (i = 0; i < inst->len; i++) {
        subids[i] = (oid)inst->subid[i];
    }

    /*
     * parse_oid_indexes() consumes the sub-identifiers into the typed
     * varbinds and fails unless they are consumed exactly, so this doubles as
     * the check that the instance really is a row of this table.
     */
    if (parse_oid_indexes(subids, inst->len, put_index_data) != SNMPERR_SUCCESS) {
        return -1;
    }
    return 0;
}

int
table_instance_from_indexes(const netsnmp_table_request_info *table_info,
                            table_instance_t *out)
{
    oid    subids[TABLE_ROW_SUBID_MAX];
    size_t len = TABLE_ROW_SUBID_MAX;
    size_t i;

    if (table_info == NULL || table_info->indexes == NULL || out == NULL) {
        return -1;
    }

    if (build_oid_noalloc(subids, TABLE_ROW_SUBID_MAX, &len, NULL, 0,
                          table_info->indexes) != SNMPERR_SUCCESS) {
        return -1;
    }
    if (len == 0 || len > TABLE_ROW_SUBID_MAX) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    for (i = 0; i < len; i++) {
        /* oid is a 64 bit unsigned long here, an SNMP sub-identifier is 32
         * bits; refuse anything that would silently truncate. */
        if (subids[i] > 0xFFFFFFFFul) {
            return -1;
        }
        out->subid[i] = (uint32_t)subids[i];
    }
    out->len = len;
    return 0;
}
