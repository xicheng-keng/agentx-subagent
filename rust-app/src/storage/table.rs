//! Conceptual-table cells in the shared key space (docs/design.md 3.2).
//!
//! A table has no schema in either LMDB environment. Each cell is an ordinary
//! key/value pair whose key is
//!
//! ```text
//! "<columnName>.<instance>"
//! ```
//!
//! where `<instance>` is the row's index sub-identifiers in dotted decimal,
//! verbatim as they appear on the wire after the column OID. This module is
//! the Rust half of `include/table_rows.h`; the two must agree on that
//! spelling exactly, because the C subagent discovers which rows exist by
//! scanning these keys, and a key it cannot parse is a row it will not report
//! (it logs and skips one). In particular the instance must be canonical: no
//! leading zeros, no empty components.

use super::{CacheStore, Result, StorageError, Value};

/// Errors specific to composing or reading table cell keys.
#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum TableKeyError {
    #[error("column name must not be empty")]
    EmptyColumn,
    #[error("column name {0:?} must not contain '.'")]
    DottedColumn(String),
    #[error("a row instance must have at least one sub-identifier")]
    EmptyInstance,
}

/// Builds the cell key for `column` at `instance`.
///
/// ```
/// # use agentx_rust_app::storage::table::cell_key;
/// assert_eq!(cell_key("portDescr", &[3]).unwrap(), "portDescr.3");
/// assert_eq!(cell_key("fooBar", &[2, 7]).unwrap(), "fooBar.2.7");
/// ```
pub fn cell_key(column: &str, instance: &[u32]) -> std::result::Result<String, TableKeyError> {
    if column.is_empty() {
        return Err(TableKeyError::EmptyColumn);
    }
    if column.contains('.') {
        // A dot in the column name would make the key ambiguous: the C side
        // splits on the first dot after the column prefix it is scanning for.
        return Err(TableKeyError::DottedColumn(column.to_string()));
    }
    if instance.is_empty() {
        return Err(TableKeyError::EmptyInstance);
    }

    let mut key = String::with_capacity(column.len() + 4 * instance.len());
    key.push_str(column);
    for sub in instance {
        key.push('.');
        key.push_str(&sub.to_string());
    }
    Ok(key)
}

/// Parses the instance out of a cell key that starts with `column`.
///
/// Returns `None` when the key belongs to another object, or when the
/// instance is not canonical dotted decimal -- the same keys the C side skips
/// (see `table_instance_parse` in `src/table_rows.c`).
pub fn instance_of(column: &str, key: &str) -> Option<Vec<u32>> {
    let rest = key.strip_prefix(column)?.strip_prefix('.')?;
    parse_instance(rest)
}

fn parse_instance(dotted: &str) -> Option<Vec<u32>> {
    if dotted.is_empty() {
        return None;
    }
    let mut out = Vec::new();
    for part in dotted.split('.') {
        if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        if part.len() > 1 && part.starts_with('0') {
            return None; // non-canonical: "007" would alias row 7
        }
        out.push(part.parse::<u32>().ok()?);
    }
    Some(out)
}

impl CacheStore {
    /// Writes every cell of one row in a single write transaction.
    ///
    /// Atomicity matters here in a way it does not for a scalar: the subagent
    /// reads each cell separately, and a row half written across two
    /// transactions is visible to a concurrent walk as a row with holes
    /// (reported as `noSuchInstance` for the missing columns). One
    /// transaction per row makes a new row appear complete or not at all.
    ///
    /// `cells` pairs a column name with the value for this row; the caller
    /// picks value types that match the MIB's syntax for each column, since
    /// the generated handler reads a fixed type per column and reports a type
    /// mismatch as `genErr`.
    pub fn put_row(&self, instance: &[u32], cells: &[(&str, Value)]) -> Result<()> {
        let mut encoded = Vec::with_capacity(cells.len());
        for (column, value) in cells {
            let key = cell_key(column, instance).map_err(StorageError::TableKey)?;
            encoded.push((key, value.encode()));
        }
        self.put_many_raw(&encoded)
    }

    /// Removes every listed column's cell for `instance`, in one transaction.
    ///
    /// Rows of a telemetry table exist exactly while their cells do, so this
    /// is how a sensor that went away stops being reported. Columns without a
    /// cell are not an error.
    pub fn delete_row(&self, instance: &[u32], columns: &[&str]) -> Result<()> {
        let mut keys = Vec::with_capacity(columns.len());
        for column in columns {
            keys.push(cell_key(column, instance).map_err(StorageError::TableKey)?);
        }
        self.delete_many_raw(&keys)
    }

    /// The row instances present under any of `columns`, sorted, deduplicated.
    ///
    /// This mirrors `table_rowset_load()` on the C side, including the
    /// ordering: sub-identifier by sub-identifier, so row 10 comes after row
    /// 2 even though the keys sort the other way bytewise. Keys whose
    /// instance is not canonical are skipped, exactly as the subagent skips
    /// them.
    pub fn table_rows(&self, columns: &[&str]) -> Result<Vec<Vec<u32>>> {
        let mut rows: Vec<Vec<u32>> = Vec::new();
        for column in columns {
            for key in self.keys_with_prefix(&format!("{column}."))? {
                if let Some(instance) = instance_of(column, &key) {
                    rows.push(instance);
                }
            }
        }
        rows.sort();
        rows.dedup();
        Ok(rows)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cell_key_matches_the_c_spelling() {
        assert_eq!(cell_key("portDescr", &[3]).unwrap(), "portDescr.3");
        assert_eq!(cell_key("cell", &[2, 7]).unwrap(), "cell.2.7");
        assert_eq!(cell_key("x", &[0]).unwrap(), "x.0");
        assert_eq!(
            cell_key("x", &[u32::MAX]).unwrap(),
            format!("x.{}", u32::MAX)
        );
    }

    #[test]
    fn cell_key_rejects_keys_the_c_side_could_not_parse() {
        assert_eq!(cell_key("", &[1]), Err(TableKeyError::EmptyColumn));
        assert_eq!(
            cell_key("a.b", &[1]),
            Err(TableKeyError::DottedColumn("a.b".into()))
        );
        assert_eq!(cell_key("x", &[]), Err(TableKeyError::EmptyInstance));
    }

    #[test]
    fn instance_of_round_trips_and_rejects_non_canonical() {
        assert_eq!(instance_of("portDescr", "portDescr.3"), Some(vec![3]));
        assert_eq!(instance_of("cell", "cell.2.7"), Some(vec![2, 7]));

        assert_eq!(instance_of("portDescr", "portDescr.007"), None);
        assert_eq!(instance_of("portDescr", "portDescr.abc"), None);
        assert_eq!(instance_of("portDescr", "portDescr."), None);
        assert_eq!(instance_of("portDescr", "portDescr"), None);
        assert_eq!(instance_of("portDescr", "portDescr.1."), None);
        assert_eq!(instance_of("portDescr", "portDescrExtra.1"), None);
        assert_eq!(instance_of("portDescr", "portDescr.4294967296"), None);
    }

    #[test]
    fn instances_sort_numerically_not_bytewise() {
        let mut rows = vec![vec![10u32], vec![2u32], vec![2u32, 1u32]];
        rows.sort();
        assert_eq!(rows, vec![vec![2], vec![2, 1], vec![10]]);
    }
}
