//! Table-cell tests for the Rust half of the shared key space
//! (docs/design.md 3.2).
//!
//! The unit tests next to the code cover key spelling; these cover the parts
//! that need a real environment: writing a row atomically, discovering rows
//! back out of the key space in the order the subagent expects, and rows
//! disappearing when their cells are deleted.

use agentx_rust_app::storage::table::cell_key;
use agentx_rust_app::storage::{CacheStore, Value};

const COLUMNS: [&str; 3] = ["sensorName", "sensorTempMilliC", "sensorSampleCount"];

fn row(name: &str, temp: i32, count: u32) -> Vec<(&'static str, Value)> {
    vec![
        ("sensorName", Value::Bytes(name.as_bytes().to_vec())),
        ("sensorTempMilliC", Value::Int32(temp)),
        ("sensorSampleCount", Value::Uint32(count)),
    ]
}

#[test]
fn put_row_writes_every_cell_under_its_own_key() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_row(&[2], &row("nvme0", 41_000, 9)).unwrap();

    assert_eq!(cache.get_bytes("sensorName.2").unwrap(), b"nvme0");
    assert_eq!(cache.get_int32("sensorTempMilliC.2").unwrap(), 41_000);
    assert_eq!(cache.get_uint32("sensorSampleCount.2").unwrap(), 9);
}

#[test]
fn table_rows_are_deduplicated_and_ordered_numerically() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_row(&[10], &row("tenth", 40_000, 1)).unwrap();
    cache.put_row(&[2], &row("second", 41_000, 1)).unwrap();
    cache.put_row(&[1], &row("first", 42_000, 1)).unwrap();

    // Bytewise, "sensorName.10" sorts before "sensorName.2"; the row order
    // the subagent reports is numeric, and this side has to agree.
    assert_eq!(
        cache.table_rows(&COLUMNS).unwrap(),
        vec![vec![1], vec![2], vec![10]]
    );
}

#[test]
fn a_row_exists_as_long_as_any_of_its_cells_does() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    // A partial row -- exactly what the subagent reports as a row with holes.
    cache
        .put_bytes(&cell_key("sensorName", &[7]).unwrap(), b"partial")
        .unwrap();

    assert_eq!(cache.table_rows(&COLUMNS).unwrap(), vec![vec![7]]);

    cache.delete_row(&[7], &COLUMNS).unwrap();
    assert!(cache.table_rows(&COLUMNS).unwrap().is_empty());
}

#[test]
fn delete_row_is_idempotent_and_leaves_other_rows_alone() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_row(&[1], &row("keep", 40_000, 1)).unwrap();
    cache.put_row(&[2], &row("drop", 41_000, 1)).unwrap();

    cache.delete_row(&[2], &COLUMNS).unwrap();
    cache.delete_row(&[2], &COLUMNS).unwrap(); // absent cells are not an error

    assert_eq!(cache.table_rows(&COLUMNS).unwrap(), vec![vec![1]]);
    assert_eq!(cache.get_bytes("sensorName.1").unwrap(), b"keep");
}

#[test]
fn keys_that_are_not_canonical_instances_are_not_rows() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_bytes("sensorName.1", b"ok").unwrap();
    cache.put_bytes("sensorName.007", b"leading zero").unwrap();
    cache.put_bytes("sensorName.abc", b"not a number").unwrap();
    cache
        .put_bytes("sensorName", b"scalar-looking key")
        .unwrap();

    // The subagent skips these keys when enumerating rows; reporting them
    // here would make the two sides disagree about what exists.
    assert_eq!(cache.table_rows(&["sensorName"]).unwrap(), vec![vec![1]]);
}

#[test]
fn multi_subidentifier_instances_round_trip() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_row(&[2, 10], &row("a", 1, 1)).unwrap();
    cache.put_row(&[2, 2], &row("b", 1, 1)).unwrap();
    cache.put_row(&[1, 9], &row("c", 1, 1)).unwrap();

    assert_eq!(
        cache.table_rows(&COLUMNS).unwrap(),
        vec![vec![1, 9], vec![2, 2], vec![2, 10]]
    );
    assert_eq!(cache.get_bytes("sensorName.2.10").unwrap(), b"a");
}
