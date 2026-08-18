//! Typed LMDB layer matching `include/storage_lmdb.h` byte for byte.
//!
//! Two environment wrappers are provided, mirroring docs/design.md ch.2.2/2.5:
//!
//! - [`CacheStore`]: read-write, opened with `MDB_NOSYNC`, for `cache.lmdb`.
//!   The Rust telemetry app is the single writer of the telemetry keys it
//!   owns there.
//! - [`ConfigStore`]: **read-only**, for `config.lmdb`. Per design.md 2.5,
//!   the C subagent is the sole writer of `config.lmdb` (SNMP Set is the
//!   only legitimate trigger for those writes); this type exposes no `put`
//!   method at all, so it is impossible to write config from this crate
//!   without deleting code, and the underlying LMDB environment is also
//!   opened with `MDB_RDONLY` as defense in depth.

pub mod codec;
pub mod table;

pub use codec::{CodecError, StorageType, Value};
pub use table::TableKeyError;

use heed::types::{Bytes, Str};
use heed::{Database, Env, EnvFlags, EnvOpenOptions};
use std::path::Path;
use thiserror::Error;

/// Default LMDB map size, matching `STORAGE_DEFAULT_MAPSIZE` in
/// `storage_lmdb.h`.
pub const DEFAULT_MAPSIZE: usize = 64 * 1024 * 1024;

/// Errors surfaced by the storage layer.
#[derive(Debug, Error)]
pub enum StorageError {
    #[error("lmdb error: {0}")]
    Lmdb(#[from] heed::Error),
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("value codec error: {0}")]
    Codec(#[from] CodecError),
    #[error("key not found: {0}")]
    NotFound(String),
    #[error("stored value for {key} has type {actual:?}, expected {expected:?}")]
    TypeMismatch {
        key: String,
        expected: StorageType,
        actual: StorageType,
    },
    #[error("the requested database does not exist in this environment")]
    MissingDatabase,
    #[error("table cell key error: {0}")]
    TableKey(#[from] TableKeyError),
}

pub type Result<T> = std::result::Result<T, StorageError>;

type Kv = Database<Str, Bytes>;

/// Shared plumbing behind both [`CacheStore`] and [`ConfigStore`].
struct Inner {
    env: Env,
    db: Kv,
}

impl Inner {
    fn get_raw(&self, key: &str) -> Result<Option<Vec<u8>>> {
        let rtxn = self.env.read_txn()?;
        let v = self.db.get(&rtxn, key)?.map(|b| b.to_vec());
        Ok(v)
    }

    fn get_value(&self, key: &str) -> Result<Value> {
        let raw = self
            .get_raw(key)?
            .ok_or_else(|| StorageError::NotFound(key.to_string()))?;
        Ok(Value::decode(&raw)?)
    }

    fn get_typed(&self, key: &str, expected: StorageType) -> Result<Value> {
        let v = self.get_value(key)?;
        if v.storage_type() != expected {
            return Err(StorageError::TypeMismatch {
                key: key.to_string(),
                expected,
                actual: v.storage_type(),
            });
        }
        Ok(v)
    }

    fn put_raw(&self, key: &str, value: &Value) -> Result<()> {
        let mut wtxn = self.env.write_txn()?;
        self.db.put(&mut wtxn, key, &value.encode())?;
        wtxn.commit()?;
        Ok(())
    }

    /// Writes several already-encoded entries in one transaction, so a reader
    /// never sees a partially written group. Used for table rows, where the
    /// group is one row's cells (see [`table`]).
    fn put_many_raw(&self, entries: &[(String, Vec<u8>)]) -> Result<()> {
        let mut wtxn = self.env.write_txn()?;
        for (key, encoded) in entries {
            self.db.put(&mut wtxn, key, encoded)?;
        }
        wtxn.commit()?;
        Ok(())
    }

    /// Deletes several keys in one transaction; absent keys are not an error.
    fn delete_many_raw(&self, keys: &[String]) -> Result<()> {
        let mut wtxn = self.env.write_txn()?;
        for key in keys {
            self.db.delete(&mut wtxn, key)?;
        }
        wtxn.commit()?;
        Ok(())
    }

    /// Every key starting with `prefix`, in LMDB (bytewise) key order.
    ///
    /// Keys are copied out rather than borrowed: the read transaction ends
    /// with this call, and holding one open across a caller's work would pin
    /// pages the writer then cannot reuse.
    fn keys_with_prefix(&self, prefix: &str) -> Result<Vec<String>> {
        let rtxn = self.env.read_txn()?;
        let mut keys = Vec::new();
        for entry in self.db.prefix_iter(&rtxn, prefix)? {
            let (key, _) = entry?;
            keys.push(key.to_string());
        }
        Ok(keys)
    }
}

/// Read-write handle onto `cache.lmdb`. See module docs.
pub struct CacheStore {
    inner: Inner,
}

impl CacheStore {
    /// Open (creating if necessary) the cache environment at `path`.
    ///
    /// `path` is a directory holding `data.mdb`/`lock.mdb`; it is created if
    /// missing. Opened with `MDB_NOSYNC` per docs/design.md 2.2.
    pub fn open(path: &Path, mapsize: Option<usize>) -> Result<Self> {
        std::fs::create_dir_all(path)?;
        let mut options = EnvOpenOptions::new();
        options
            .map_size(mapsize.unwrap_or(DEFAULT_MAPSIZE))
            .max_dbs(1);
        // SAFETY: NO_SYNC is an accepted "unsafe" LMDB flag per heed's API;
        // cache.lmdb is volatile scratch data on tmpfs (design.md 2.2), so
        // losing the last few writes on a crash is an accepted trade-off.
        unsafe {
            options.flags(EnvFlags::NO_SYNC);
        }
        let env = unsafe { options.open(path)? };
        let mut wtxn = env.write_txn()?;
        let db: Kv = env.create_database(&mut wtxn, None)?;
        wtxn.commit()?;
        Ok(Self {
            inner: Inner { env, db },
        })
    }

    /// Fetch a raw [`Value`] by key.
    pub fn get(&self, key: &str) -> Result<Value> {
        self.inner.get_value(key)
    }

    pub fn get_int32(&self, key: &str) -> Result<i32> {
        match self.inner.get_typed(key, StorageType::Int32)? {
            Value::Int32(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_uint32(&self, key: &str) -> Result<u32> {
        match self.inner.get_typed(key, StorageType::Uint32)? {
            Value::Uint32(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_uint64(&self, key: &str) -> Result<u64> {
        match self.inner.get_typed(key, StorageType::Uint64)? {
            Value::Uint64(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_bytes(&self, key: &str) -> Result<Vec<u8>> {
        match self.inner.get_typed(key, StorageType::Bytes)? {
            Value::Bytes(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_oid(&self, key: &str) -> Result<Vec<u32>> {
        match self.inner.get_typed(key, StorageType::Oid)? {
            Value::Oid(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    /// Returns `Ok(None)` if `key` is absent instead of `NotFound`.
    pub fn try_get(&self, key: &str) -> Result<Option<Value>> {
        match self.inner.get_raw(key)? {
            Some(raw) => Ok(Some(Value::decode(&raw)?)),
            None => Ok(None),
        }
    }

    pub fn put(&self, key: &str, value: &Value) -> Result<()> {
        self.inner.put_raw(key, value)
    }

    pub fn put_int32(&self, key: &str, v: i32) -> Result<()> {
        self.put(key, &Value::Int32(v))
    }

    pub fn put_uint32(&self, key: &str, v: u32) -> Result<()> {
        self.put(key, &Value::Uint32(v))
    }

    pub fn put_uint64(&self, key: &str, v: u64) -> Result<()> {
        self.put(key, &Value::Uint64(v))
    }

    pub fn put_bytes(&self, key: &str, v: &[u8]) -> Result<()> {
        self.put(key, &Value::Bytes(v.to_vec()))
    }

    pub fn put_oid(&self, key: &str, v: &[u32]) -> Result<()> {
        self.put(key, &Value::Oid(v.to_vec()))
    }

    /// Removes `key`; absent keys are not an error.
    pub fn delete(&self, key: &str) -> Result<()> {
        self.inner.delete_many_raw(&[key.to_string()])
    }

    pub(crate) fn put_many_raw(&self, entries: &[(String, Vec<u8>)]) -> Result<()> {
        self.inner.put_many_raw(entries)
    }

    pub(crate) fn delete_many_raw(&self, keys: &[String]) -> Result<()> {
        self.inner.delete_many_raw(keys)
    }

    pub(crate) fn keys_with_prefix(&self, prefix: &str) -> Result<Vec<String>> {
        self.inner.keys_with_prefix(prefix)
    }
}

/// Read-only handle onto `config.lmdb`.
///
/// docs/design.md 2.5: "SNMP Setで変更される値...Cサブエージェントを唯一のライター
/// とする（config.lmdb）" — the Rust app must never open `config.lmdb`
/// writable. This type intentionally exposes no `put*` method; the only way
/// to change a config value from this crate is via [`crate::ipc::IpcClient`],
/// which asks the C subagent to perform the write.
pub struct ConfigStore {
    inner: Inner,
}

impl ConfigStore {
    /// Open `config.lmdb` at `path` in `MDB_RDONLY` mode.
    ///
    /// The environment (and its `data.mdb`) must already exist; unlike
    /// [`CacheStore::open`], this does not create the directory or the
    /// database, since a config store must never conjure config out of
    /// nothing on the Rust side.
    pub fn open(path: &Path, mapsize: Option<usize>) -> Result<Self> {
        let mut options = EnvOpenOptions::new();
        options
            .map_size(mapsize.unwrap_or(DEFAULT_MAPSIZE))
            .max_dbs(1);
        // SAFETY: READ_ONLY is one of heed's "unsafe" LMDB flags because the
        // caller must not attempt writes against the resulting Env; this
        // type upholds that by never exposing a write transaction.
        unsafe {
            options.flags(EnvFlags::READ_ONLY);
        }
        let env = unsafe { options.open(path)? };
        let rtxn = env.read_txn()?;
        let db: Kv = env
            .open_database(&rtxn, None)?
            .ok_or(StorageError::MissingDatabase)?;
        drop(rtxn);
        Ok(Self {
            inner: Inner { env, db },
        })
    }

    pub fn get(&self, key: &str) -> Result<Value> {
        self.inner.get_value(key)
    }

    pub fn try_get(&self, key: &str) -> Result<Option<Value>> {
        match self.inner.get_raw(key)? {
            Some(raw) => Ok(Some(Value::decode(&raw)?)),
            None => Ok(None),
        }
    }

    pub fn get_int32(&self, key: &str) -> Result<i32> {
        match self.inner.get_typed(key, StorageType::Int32)? {
            Value::Int32(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_uint32(&self, key: &str) -> Result<u32> {
        match self.inner.get_typed(key, StorageType::Uint32)? {
            Value::Uint32(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_uint64(&self, key: &str) -> Result<u64> {
        match self.inner.get_typed(key, StorageType::Uint64)? {
            Value::Uint64(v) => Ok(v),
            _ => unreachable!(),
        }
    }

    pub fn get_bytes(&self, key: &str) -> Result<Vec<u8>> {
        match self.inner.get_typed(key, StorageType::Bytes)? {
            Value::Bytes(v) => Ok(v),
            _ => unreachable!(),
        }
    }
}
