//! it_helper -- small CLI used only by the bash integration scenarios under
//! tests/ (scripts/run-integration.sh). It exercises the Rust side of the
//! design independently of the C subagent:
//!
//!   - `config-hex <dir> <key>`   : open config.lmdb read-only (ConfigStore,
//!                                  exactly as the design mandates the Rust
//!                                  app must) and print the *encoded* bytes
//!                                  for <key> as lowercase hex. Compared
//!                                  byte-for-byte against `storage_cli
//!                                  hexdump` (the C side) in scenario (d) to
//!                                  prove the two independent encoders agree.
//!   - `cache-get-int32 <dir> <key>` / `cache-get-uint64 <dir> <key>` :
//!                                  read a value out of cache.lmdb.
//!   - `cache-watch <dir> <key> <iterations> <delay_ms>` : read the same key
//!                                  repeatedly and report how many reads
//!                                  succeeded vs failed to decode, to catch
//!                                  torn/partial reads while a writer is
//!                                  concurrently committing (scenario b).
//!   - `ipc-write <sock> <key> <kind> <value>` / `ipc-read <sock> <key>` /
//!     `ipc-ping <sock> <nonce>` : thin wrappers over IpcClient, used by the
//!                                  IPC scenario (e) for concurrent/invalid
//!                                  request checks beyond what the `loadgen`
//!                                  binary already covers.
//!
//! This crate is test-only tooling; it is not part of the product and does
//! not modify anything under rust-app/.

use agentx_rust_app::ipc::{IpcClient, IpcValue};
use agentx_rust_app::storage::{CacheStore, ConfigStore};
use std::path::PathBuf;
use std::process::ExitCode;

fn print_hex(bytes: &[u8]) {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    println!("{s}");
}

fn cmd_config_hex(dir: &str, key: &str) -> ExitCode {
    let store = match ConfigStore::open(&PathBuf::from(dir), None) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("config-hex: open failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    match store.get(key) {
        Ok(v) => {
            print_hex(&v.encode());
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("config-hex: get({key}) failed: {e}");
            ExitCode::FAILURE
        }
    }
}

fn cmd_cache_hex(dir: &str, key: &str) -> ExitCode {
    let store = match CacheStore::open(&PathBuf::from(dir), None) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cache-hex: open failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    match store.get(key) {
        Ok(v) => {
            print_hex(&v.encode());
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("cache-hex: get({key}) failed: {e}");
            ExitCode::FAILURE
        }
    }
}

fn cmd_cache_watch(dir: &str, key: &str, iterations: u64, delay_ms: u64) -> ExitCode {
    let store = match CacheStore::open(&PathBuf::from(dir), None) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cache-watch: open failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    let mut ok = 0u64;
    let mut notfound = 0u64;
    let mut errors = 0u64;
    for i in 0..iterations {
        match store.try_get(key) {
            Ok(Some(_)) => ok += 1,
            Ok(None) => notfound += 1,
            Err(e) => {
                errors += 1;
                eprintln!("cache-watch: iter {i} decode error: {e}");
            }
        }
        if delay_ms > 0 {
            std::thread::sleep(std::time::Duration::from_millis(delay_ms));
        }
    }
    println!("ok={ok} notfound={notfound} errors={errors}");
    if errors > 0 {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

fn parse_ipc_value(kind: &str, value: &str) -> Result<IpcValue, String> {
    Ok(match kind {
        "int32" => IpcValue::Int32(value.parse().map_err(|e| format!("{e}"))?),
        "uint32" => IpcValue::Uint32(value.parse().map_err(|e| format!("{e}"))?),
        "uint64" => IpcValue::Uint64(value.parse().map_err(|e| format!("{e}"))?),
        "bytes" => IpcValue::Bytes(value.as_bytes().to_vec()),
        other => return Err(format!("unknown ipc value kind '{other}'")),
    })
}

fn cmd_ipc_write(sock: &str, key: &str, kind: &str, value: &str) -> ExitCode {
    let ipc_value = match parse_ipc_value(kind, value) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("ipc-write: {e}");
            return ExitCode::FAILURE;
        }
    };
    let mut client = match IpcClient::connect(sock) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("ipc-write: connect failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    match client.write_config(key, ipc_value) {
        Ok(resp) => {
            println!("status={} message={:?}", resp.status, resp.message);
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("ipc-write: {e}");
            ExitCode::FAILURE
        }
    }
}

fn cmd_ipc_read(sock: &str, key: &str) -> ExitCode {
    let mut client = match IpcClient::connect(sock) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("ipc-read: connect failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    match client.read_config(key) {
        Ok(resp) => {
            println!("status={} message={:?}", resp.status, resp.message);
            if let Some(v) = resp.value {
                println!(
                    "type={} int32={} uint32={} uint64={} bytes_len={}",
                    v.r#type,
                    v.int32_val,
                    v.uint32_val,
                    v.uint64_val,
                    v.bytes_val.len()
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("ipc-read: {e}");
            ExitCode::FAILURE
        }
    }
}

fn cmd_ipc_ping(sock: &str, nonce: u32) -> ExitCode {
    let mut client = match IpcClient::connect(sock) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("ipc-ping: connect failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    match client.ping(nonce) {
        Ok(echoed) if echoed == nonce => {
            println!("ping ok nonce={echoed}");
            ExitCode::SUCCESS
        }
        Ok(echoed) => {
            eprintln!("ping: nonce mismatch, sent {nonce} got {echoed}");
            ExitCode::FAILURE
        }
        Err(e) => {
            eprintln!("ipc-ping: {e}");
            ExitCode::FAILURE
        }
    }
}

/// Sends a deliberately malformed frame (bad length prefix) directly on the
/// socket, bypassing IpcClient's well-formed framing, to check the server
/// rejects it (closing/erroring this connection) without disturbing other
/// clients. Scenario (e).
fn cmd_ipc_send_garbage(sock: &str) -> ExitCode {
    use std::io::Write;
    use std::os::unix::net::UnixStream;
    let mut stream = match UnixStream::connect(sock) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("ipc-send-garbage: connect failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    // Claim a frame far larger than IPC_MAX_FRAME_LEN (64 KiB) but only send
    // a few bytes -- either the server enforces the length cap and closes
    // the connection, or it hangs waiting for more bytes than we send
    // (which the caller should time out on, also a valid "not corrupted
    // silently" outcome).
    let bogus_len: u32 = 10 * 1024 * 1024;
    let _ = stream.write_all(&bogus_len.to_be_bytes());
    let _ = stream.write_all(b"short");
    let _ = stream.flush();
    println!("garbage frame sent");
    ExitCode::SUCCESS
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: it_helper <subcommand> ...\n\
         \n\
         config-hex <config_dir> <key>\n\
         cache-hex <cache_dir> <key>\n\
         cache-watch <cache_dir> <key> <iterations> <delay_ms>\n\
         ipc-write <sock> <key> <int32|uint32|uint64|bytes> <value>\n\
         ipc-read <sock> <key>\n\
         ipc-ping <sock> <nonce>\n\
         ipc-send-garbage <sock>"
    );
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        return usage();
    }
    match args[1].as_str() {
        "config-hex" if args.len() == 4 => cmd_config_hex(&args[2], &args[3]),
        "cache-hex" if args.len() == 4 => cmd_cache_hex(&args[2], &args[3]),
        "cache-watch" if args.len() == 6 => cmd_cache_watch(
            &args[2],
            &args[3],
            args[4].parse().unwrap_or(0),
            args[5].parse().unwrap_or(0),
        ),
        "ipc-write" if args.len() == 6 => cmd_ipc_write(&args[2], &args[3], &args[4], &args[5]),
        "ipc-read" if args.len() == 4 => cmd_ipc_read(&args[2], &args[3]),
        "ipc-ping" if args.len() == 4 => cmd_ipc_ping(&args[2], args[3].parse().unwrap_or(0)),
        "ipc-send-garbage" if args.len() == 3 => cmd_ipc_send_garbage(&args[2]),
        _ => usage(),
    }
}
