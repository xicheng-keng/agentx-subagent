/*
 * table_rows.c — implementation of include/table_rows.h.
 *
 * Row discovery is a prefix scan per column (storage_iter_*), a sort, and a
 * de-duplication pass. See the header for the key format and the ordering
 * contract, and docs/design.md 3.2 for why rows are derived from the key
 * space instead of being declared anywhere.
 */
#include "table_rows.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct table_rowset {
    table_instance_t *rows;
    size_t            count;
    size_t            capacity;
    size_t            skipped;
};

/* --------------------------------------------------------------------- */
/* instance <-> key                                                      */
/* --------------------------------------------------------------------- */

storage_rc_t table_instance_format(const table_instance_t *inst,
                                    char *buf, size_t buflen)
{
    if (inst == NULL || buf == NULL || buflen == 0) {
        return STORAGE_ERR_INVAL;
    }
    if (inst->len == 0 || inst->len > TABLE_ROW_SUBID_MAX) {
        return STORAGE_ERR_INVAL;
    }

    size_t used = 0;
    for (size_t i = 0; i < inst->len; i++) {
        int n = snprintf(buf + used, buflen - used, "%s%lu",
                          (i == 0) ? "" : ".", (unsigned long)inst->subid[i]);
        if (n < 0) {
            return STORAGE_ERR_INVAL;
        }
        if ((size_t)n >= buflen - used) {
            return STORAGE_ERR_TOOBIG;
        }
        used += (size_t)n;
    }
    return STORAGE_OK;
}

storage_rc_t table_instance_parse(const char *dotted, size_t len,
                                   table_instance_t *out)
{
    if (dotted == NULL || out == NULL || len == 0) {
        return STORAGE_ERR_INVAL;
    }

    table_instance_t inst;
    inst.len = 0;

    size_t pos = 0;
    while (pos < len) {
        if (inst.len == TABLE_ROW_SUBID_MAX) {
            return STORAGE_ERR_INVAL;
        }

        size_t start = pos;
        uint64_t value = 0;
        while (pos < len && dotted[pos] >= '0' && dotted[pos] <= '9') {
            value = value * 10u + (uint64_t)(dotted[pos] - '0');
            if (value > 0xFFFFFFFFu) {
                return STORAGE_ERR_INVAL;
            }
            pos++;
        }
        size_t digits = pos - start;
        if (digits == 0) {
            return STORAGE_ERR_INVAL; /* empty component, or a non-digit */
        }
        if (digits > 1 && dotted[start] == '0') {
            return STORAGE_ERR_INVAL; /* non-canonical leading zero */
        }

        inst.subid[inst.len++] = (uint32_t)value;

        if (pos == len) {
            break;
        }
        if (dotted[pos] != '.') {
            return STORAGE_ERR_INVAL; /* stray character */
        }
        pos++;
        if (pos == len) {
            return STORAGE_ERR_INVAL; /* trailing '.' */
        }
    }

    /* Zero the unused tail so two instances that compare equal also memcmp
     * equal, which keeps the de-duplication pass honest. */
    memset(inst.subid + inst.len, 0,
            (TABLE_ROW_SUBID_MAX - inst.len) * sizeof(inst.subid[0]));
    *out = inst;
    return STORAGE_OK;
}

storage_rc_t table_cell_key(const char *column, const table_instance_t *inst,
                             char *buf, size_t buflen)
{
    if (column == NULL || buf == NULL) {
        return STORAGE_ERR_INVAL;
    }
    size_t collen = strlen(column);
    if (collen == 0) {
        return STORAGE_ERR_INVAL;
    }
    if (collen + 2 > buflen) { /* column + '.' + at least one digit + NUL */
        return STORAGE_ERR_TOOBIG;
    }

    memcpy(buf, column, collen);
    buf[collen] = '.';

    storage_rc_t rc = table_instance_format(inst, buf + collen + 1,
                                             buflen - collen - 1);
    if (rc != STORAGE_OK) {
        return rc;
    }
    if (strlen(buf) > STORAGE_KEY_MAX) {
        return STORAGE_ERR_TOOBIG;
    }
    return STORAGE_OK;
}

int table_instance_compare(const table_instance_t *a, const table_instance_t *b)
{
    size_t shared = (a->len < b->len) ? a->len : b->len;

    for (size_t i = 0; i < shared; i++) {
        if (a->subid[i] != b->subid[i]) {
            return (a->subid[i] < b->subid[i]) ? -1 : 1;
        }
    }
    if (a->len == b->len) {
        return 0;
    }
    return (a->len < b->len) ? -1 : 1;
}

/* --------------------------------------------------------------------- */
/* rowsets                                                               */
/* --------------------------------------------------------------------- */

static int compare_instances(const void *lhs, const void *rhs)
{
    return table_instance_compare((const table_instance_t *)lhs,
                                   (const table_instance_t *)rhs);
}

static storage_rc_t rowset_push(table_rowset_t *rs, const table_instance_t *inst)
{
    if (rs->count == rs->capacity) {
        size_t next = (rs->capacity == 0) ? 16 : rs->capacity * 2;
        table_instance_t *grown = realloc(rs->rows, next * sizeof(*grown));

        if (grown == NULL) {
            return STORAGE_ERR_IO;
        }
        rs->rows = grown;
        rs->capacity = next;
    }
    rs->rows[rs->count++] = *inst;
    return STORAGE_OK;
}

static storage_rc_t rowset_load_column(table_rowset_t *rs,
                                        const table_column_ref_t *col)
{
    if (col->env == NULL || col->column == NULL || col->column[0] == '\0') {
        return STORAGE_ERR_INVAL;
    }

    char prefix[STORAGE_KEY_MAX + 2];
    int n = snprintf(prefix, sizeof(prefix), "%s.", col->column);

    if (n < 0 || (size_t)n >= sizeof(prefix)) {
        return STORAGE_ERR_TOOBIG;
    }
    size_t prefix_len = (size_t)n;

    storage_iter_t *it = NULL;
    storage_rc_t rc = storage_iter_open(col->env, prefix, &it);

    if (rc != STORAGE_OK) {
        return rc;
    }

    for (;;) {
        const char *key = NULL;
        size_t keylen = 0;

        rc = storage_iter_next(it, &key, &keylen);
        if (rc == STORAGE_ERR_NOTFOUND) {
            rc = STORAGE_OK;
            break;
        }
        if (rc != STORAGE_OK) {
            break;
        }

        table_instance_t inst;
        if (table_instance_parse(key + prefix_len, keylen - prefix_len,
                                  &inst) != STORAGE_OK) {
            rs->skipped++;
            continue;
        }
        rc = rowset_push(rs, &inst);
        if (rc != STORAGE_OK) {
            break;
        }
    }

    storage_iter_close(it);
    return rc;
}

storage_rc_t table_rowset_load(const table_column_ref_t *columns,
                                size_t ncolumns, table_rowset_t **out)
{
    if (columns == NULL || ncolumns == 0 || out == NULL) {
        return STORAGE_ERR_INVAL;
    }

    table_rowset_t *rs = calloc(1, sizeof(*rs));
    if (rs == NULL) {
        return STORAGE_ERR_IO;
    }

    for (size_t i = 0; i < ncolumns; i++) {
        storage_rc_t rc = rowset_load_column(rs, &columns[i]);

        if (rc != STORAGE_OK) {
            table_rowset_free(rs);
            return rc;
        }
    }

    if (rs->count > 1) {
        qsort(rs->rows, rs->count, sizeof(rs->rows[0]), compare_instances);

        size_t unique = 1;
        for (size_t i = 1; i < rs->count; i++) {
            if (table_instance_compare(&rs->rows[i], &rs->rows[unique - 1]) != 0) {
                rs->rows[unique++] = rs->rows[i];
            }
        }
        rs->count = unique;
    }

    *out = rs;
    return STORAGE_OK;
}

size_t table_rowset_count(const table_rowset_t *rs)
{
    return (rs == NULL) ? 0 : rs->count;
}

const table_instance_t *table_rowset_at(const table_rowset_t *rs, size_t index)
{
    if (rs == NULL || index >= rs->count) {
        return NULL;
    }
    return &rs->rows[index];
}

size_t table_rowset_skipped(const table_rowset_t *rs)
{
    return (rs == NULL) ? 0 : rs->skipped;
}

void table_rowset_free(table_rowset_t *rs)
{
    if (rs == NULL) {
        return;
    }
    free(rs->rows);
    free(rs);
}
