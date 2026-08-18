//! Rust-side companion to the AgentX C subagent (docs/design.md).
//!
//! - [`storage`]: typed LMDB layer, byte-compatible with `storage_lmdb.h`.
//! - [`ipc`]: client for the Unix-socket + protobuf IPC to the C subagent.

pub mod ipc;
pub mod storage;
