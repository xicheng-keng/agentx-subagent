//! Small IPC exerciser: sends a single `WriteConfigRequest` and prints the
//! response. Used by (future) cross-process integration tests to drive the
//! C subagent's IPC socket without a full test harness.

use agentx_rust_app::ipc::pb::Status;
use agentx_rust_app::ipc::{IpcClient, IpcValue};
use clap::{Parser, ValueEnum};
use std::path::PathBuf;

#[derive(Clone, Copy, Debug, ValueEnum)]
enum ValueKind {
    Int32,
    Uint32,
    Uint64,
    Bytes,
}

#[derive(Parser, Debug)]
#[command(about = "Send a WriteConfigRequest to the C subagent over its IPC socket")]
struct Args {
    /// Path to the subagent's AF_UNIX IPC socket.
    #[arg(long, default_value = "/run/agentx-subagent/ipc.sock")]
    socket: PathBuf,

    /// MIB object name to write, e.g. "sampleIntervalSec".
    #[arg(long)]
    key: String,

    /// Kind of value payload to send.
    #[arg(long, value_enum, default_value_t = ValueKind::Uint32)]
    kind: ValueKind,

    /// Value to send; parsed according to --kind (bytes: raw UTF-8 text).
    #[arg(long)]
    value: String,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();

    let ipc_value = match args.kind {
        ValueKind::Int32 => IpcValue::Int32(args.value.parse()?),
        ValueKind::Uint32 => IpcValue::Uint32(args.value.parse()?),
        ValueKind::Uint64 => IpcValue::Uint64(args.value.parse()?),
        ValueKind::Bytes => IpcValue::Bytes(args.value.into_bytes()),
    };

    let mut client = IpcClient::connect(&args.socket)?;
    let ping_nonce: u32 = rand::random();
    let echoed_nonce = client.ping(ping_nonce)?;
    println!("[loadgen] ping echoed nonce={echoed_nonce:#x}");

    let response = client.write_config(&args.key, ipc_value)?;
    let status = Status::try_from(response.status).unwrap_or(Status::Unspecified);
    println!(
        "[loadgen] write_config key={:?} status={:?} message={:?}",
        args.key, status, response.message
    );

    Ok(())
}
