/*
 * table_rows.h — row instances of a conceptual table, discovered from LMDB.
 *
 * docs/design.md 3.2: a conceptual table has no schema of its own in the
 * storage layer. Every cell is an ordinary key/value pair whose key is
 *
 *     "<columnName>.<instance>"
 *
 * where <instance> is the row's index sub-identifiers in dotted decimal,
 * verbatim as they appear on the wire after the column OID ("portDescr.3",
 * "fooBar.2.7"). Taking the instance straight off the OID is what keeps this
 * layer independent of index syntax: an integer index, a fixed length string
 * index and a multi-object index all reduce to a list of sub-identifiers, and
 * net-snmp's parse_oid_indexes()/build_oid() do the typed conversion inside
 * the generated handler where the MIB's index types are known.
 *
 * Which rows exist is therefore a property of the key space, not of a
 * declaration: a row exists as long as at least one of its cells does. A
 * rowset is the sorted, de-duplicated union of the instances found under a
 * table's column prefixes, which may span both environments, because
 * storage_mode.h picks the backend per column and a single row can straddle
 * config.lmdb and cache.lmdb.
 *
 * This header deliberately does not depend on net-snmp: keeping row discovery
 * free of the agent API is what lets tests/test_table_rows.c exercise it
 * directly against a scratch LMDB environment.
 */
#ifndef TABLE_ROWS_H
#define TABLE_ROWS_H

#include <stddef.h>
#include <stdint.h>

#include "storage_lmdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum number of sub-identifiers in one row instance. 128 covers every
 * index shape the storage key length (STORAGE_KEY_MAX) can hold anyway: an
 * implied string index spends one sub-identifier per octet, so this is the
 * limit that bites first for long string indexes.
 */
#define TABLE_ROW_SUBID_MAX 128u

/* A row instance: the index sub-identifiers of one row, in OID order. */
typedef struct {
    uint32_t subid[TABLE_ROW_SUBID_MAX];
    size_t   len;
} table_instance_t;

/*
 * One column of a table, paired with the environment that column's cells
 * live in (STORAGE_MODE_<column> decides which, per column, so the pairs
 * handed to table_rowset_load() may name different environments).
 */
typedef struct {
    storage_env_t *env;
    const char    *column;
} table_column_ref_t;

/* An immutable, sorted, de-duplicated set of row instances. */
typedef struct table_rowset table_rowset_t;

/* --- instance <-> key ------------------------------------------------- */

/*
 * Render `inst` as dotted decimal into `buf` ("3", "2.7").
 * STORAGE_ERR_TOOBIG if it does not fit, STORAGE_ERR_INVAL if the instance is
 * empty or longer than TABLE_ROW_SUBID_MAX.
 */
storage_rc_t table_instance_format(const table_instance_t *inst,
                                   char *buf, size_t buflen);

/*
 * Parse `len` bytes of dotted decimal (not required to be NUL terminated)
 * into `*out`.
 *
 * Only the canonical form is accepted: at least one component, components
 * separated by single '.', decimal digits only, no leading zeros, each
 * component <= 4294967295. Anything else is STORAGE_ERR_INVAL. Being strict
 * here is what makes the mapping bijective: "portDescr.007" would enumerate
 * row 7, whose cells the handler would then look for under "portDescr.7" and
 * not find.
 */
storage_rc_t table_instance_parse(const char *dotted, size_t len,
                                  table_instance_t *out);

/* Build the full cell key "<column>.<instance>" into `buf`. */
storage_rc_t table_cell_key(const char *column, const table_instance_t *inst,
                            char *buf, size_t buflen);

/*
 * SNMP lexicographic order over sub-identifiers: element by element, and a
 * proper prefix sorts before the longer instance. Returns <0, 0 or >0.
 *
 * This is not LMDB's key order -- bytewise, "portDescr.10" precedes
 * "portDescr.2" -- which is precisely why a rowset sorts its own contents
 * instead of relying on the order keys come off a cursor.
 */
int table_instance_compare(const table_instance_t *a, const table_instance_t *b);

/* --- rowsets ---------------------------------------------------------- */

/*
 * Load the union of row instances present under every (env, column) pair in
 * `columns`, sorted ascending with duplicates removed.
 *
 * An empty table yields an empty rowset (count 0), not an error. Keys under a
 * column prefix whose instance is not canonical are skipped and counted, see
 * table_rowset_skipped(); a foreign writer's malformed key can then be
 * reported without derailing a walk of the rows that are well formed.
 *
 * On success *out owns memory that table_rowset_free() releases. Each
 * environment is read under its own short lived read transaction, so a
 * rowset is a snapshot per column rather than one atomic snapshot of both
 * environments -- a distinction that only matters for a table whose columns
 * are split across the two, where a row being written concurrently may show
 * up with some cells still missing (handled as a hole, see docs/design.md
 * 3.2).
 */
storage_rc_t table_rowset_load(const table_column_ref_t *columns,
                               size_t ncolumns, table_rowset_t **out);

size_t                  table_rowset_count(const table_rowset_t *rs);
const table_instance_t *table_rowset_at(const table_rowset_t *rs, size_t index);
/* Number of keys skipped for a non-canonical instance during the load. */
size_t                  table_rowset_skipped(const table_rowset_t *rs);
void                    table_rowset_free(table_rowset_t *rs);

#ifdef __cplusplus
}
#endif
#endif /* TABLE_ROWS_H */
