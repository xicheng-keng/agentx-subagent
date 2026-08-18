//! Roundtrip tests for the typed LMDB layer over temporary environments.

use agentx_rust_app::storage::{CacheStore, ConfigStore, Value};
use heed::types::{Bytes, Str};
use heed::EnvOpenOptions;
use std::sync::Arc;
use std::thread;
use std::time::Duration;

/// Creates (and populates) a config-like environment directly via `heed`
/// (bypassing `ConfigStore`, which is read-only by design), so tests can
/// exercise `ConfigStore::open` against real data.
fn seed_config_env(dir: &std::path::Path, entries: &[(&str, Value)]) {
    std::fs::create_dir_all(dir).unwrap();
    let mut options = EnvOpenOptions::new();
    options
        .map_size(agentx_rust_app::storage::DEFAULT_MAPSIZE)
        .max_dbs(1);
    let env = unsafe { options.open(dir) }.unwrap();
    let mut wtxn = env.write_txn().unwrap();
    let db: heed::Database<Str, Bytes> = env.create_database(&mut wtxn, None).unwrap();
    for (k, v) in entries {
        db.put(&mut wtxn, k, &v.encode()).unwrap();
    }
    wtxn.commit().unwrap();
    // heed dedups `Env` handles per canonical path in a process-global
    // table, and that table itself holds a strong reference -- an ordinary
    // `drop(env)` therefore does not actually close it. Explicitly close so
    // `ConfigStore::open` (different flags: read-only) can reopen the same
    // path afresh instead of hitting `BadOpenOptions`.
    env.prepare_for_closing().wait();
}

#[test]
fn cache_store_roundtrip_all_types() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();

    cache.put_int32("i32key", -12345).unwrap();
    cache.put_uint32("u32key", 0xCAFEBABE).unwrap();
    cache.put_uint64("u64key", 0x0102_0304_0506_0708).unwrap();
    cache.put_bytes("byteskey", b"hello world").unwrap();
    cache.put_oid("oidkey", &[1, 3, 6, 1, 4, 1]).unwrap();

    assert_eq!(cache.get_int32("i32key").unwrap(), -12345);
    assert_eq!(cache.get_uint32("u32key").unwrap(), 0xCAFEBABE);
    assert_eq!(cache.get_uint64("u64key").unwrap(), 0x0102_0304_0506_0708);
    assert_eq!(cache.get_bytes("byteskey").unwrap(), b"hello world");
    assert_eq!(cache.get_oid("oidkey").unwrap(), vec![1, 3, 6, 1, 4, 1]);
}

#[test]
fn cache_store_missing_key_is_not_found() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();
    assert!(cache.get_int32("nope").is_err());
    assert_eq!(cache.try_get("nope").unwrap(), None);
}

#[test]
fn cache_store_type_mismatch_is_rejected() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();
    cache.put_int32("key", 7).unwrap();
    assert!(cache.get_uint32("key").is_err());
}

#[test]
fn cache_store_overwrite_updates_value() {
    let dir = tempfile::tempdir().unwrap();
    let cache = CacheStore::open(dir.path(), None).unwrap();
    cache.put_uint64("counter", 1).unwrap();
    cache.put_uint64("counter", 2).unwrap();
    assert_eq!(cache.get_uint64("counter").unwrap(), 2);
}

#[test]
fn config_store_is_read_only_and_reads_seeded_values() {
    let dir = tempfile::tempdir().unwrap();
    seed_config_env(
        dir.path(),
        &[
            ("sampleIntervalSec", Value::Uint32(30)),
            ("deviceName", Value::Bytes(b"raspi5-demo".to_vec())),
        ],
    );

    let config = ConfigStore::open(dir.path(), None).unwrap();
    assert_eq!(config.get_uint32("sampleIntervalSec").unwrap(), 30);
    assert_eq!(config.get_bytes("deviceName").unwrap(), b"raspi5-demo");

    // ConfigStore exposes no put* method at all -- this is a compile-time
    // guarantee, not just a runtime one. (No code to demonstrate here; the
    // absence of `config.put_uint32(..)` compiling is the point.)
}

#[test]
fn concurrent_readers_do_not_block_writer_and_see_consistent_snapshots() {
    let dir = tempfile::tempdir().unwrap();
    let cache = Arc::new(CacheStore::open(dir.path(), None).unwrap());
    cache.put_uint64("value", 0).unwrap();

    let writer_cache = Arc::clone(&cache);
    let writer = thread::spawn(move || {
        for i in 1..=200u64 {
            writer_cache.put_uint64("value", i).unwrap();
        }
    });

    // Readers run concurrently with the writer. Each individual read must
    // observe *some* valid, previously-committed value (MVCC snapshot
    // isolation), never a torn/partial write, and must not error out due to
    // writer contention (LMDB readers never block on a writer).
    let mut reader_handles = Vec::new();
    for _ in 0..8 {
        let reader_cache = Arc::clone(&cache);
        reader_handles.push(thread::spawn(move || {
            let mut last_seen = 0u64;
            for _ in 0..500 {
                let v = reader_cache
                    .get_uint64("value")
                    .expect("reader must not error");
                // Values only ever increase in this test, so any regression
                // would indicate a torn read.
                assert!(v >= last_seen);
                last_seen = v;
                thread::sleep(Duration::from_micros(50));
            }
            last_seen
        }));
    }

    writer.join().unwrap();
    for h in reader_handles {
        h.join().unwrap();
    }

    assert_eq!(cache.get_uint64("value").unwrap(), 200);
}
