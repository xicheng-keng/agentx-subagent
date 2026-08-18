//! Client for the Unix-domain-socket + protobuf IPC described in
//! `proto/agentx_ipc.proto` and docs/design.md ch.4.
//!
//! Framing: a 4-byte **big-endian** length prefix followed by that many
//! bytes of a serialized `Envelope`. Frames longer than [`MAX_FRAME_LEN`]
//! are a protocol error.

use prost::Message;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::sync::atomic::{AtomicU32, Ordering};
use thiserror::Error;

#[allow(clippy::all)]
pub mod pb {
    include!(concat!(env!("OUT_DIR"), "/agentx.ipc.v1.rs"));
}

use pb::envelope::Body;
use pb::{
    Envelope, PingRequest, PingResponse, ReadConfigRequest, ReadConfigResponse, SendTrapRequest,
    SendTrapResponse, Value as PbValue, ValueType, WriteConfigRequest, WriteConfigResponse,
};

/// Maximum serialized `Envelope` size, per `proto/agentx_ipc.proto`.
pub const MAX_FRAME_LEN: u32 = 64 * 1024;

/// Errors from the IPC client.
#[derive(Debug, Error)]
pub enum IpcError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("protobuf decode error: {0}")]
    Decode(#[from] prost::DecodeError),
    #[error("frame length {0} exceeds max frame length {MAX_FRAME_LEN}")]
    FrameTooLarge(u32),
    #[error("connection closed by peer before a complete frame was received")]
    ConnectionClosed,
    #[error("response request_id {got} did not match request request_id {expected}")]
    RequestIdMismatch { expected: u32, got: u32 },
    #[error("server sent an unexpected response body variant")]
    UnexpectedBody,
}

pub type Result<T> = std::result::Result<T, IpcError>;

/// A connected client for the C subagent's IPC socket.
///
/// Not `Sync`/thread-safe by itself: each call is a synchronous
/// request/response round trip over one `UnixStream`. Wrap in a `Mutex` (or
/// open one connection per thread) for concurrent use.
pub struct IpcClient {
    stream: UnixStream,
    next_request_id: AtomicU32,
}

impl IpcClient {
    /// Connect to the subagent's `AF_UNIX` socket at `path`.
    pub fn connect<P: AsRef<Path>>(path: P) -> Result<Self> {
        let stream = UnixStream::connect(path)?;
        Ok(Self {
            stream,
            next_request_id: AtomicU32::new(1),
        })
    }

    fn fresh_request_id(&self) -> u32 {
        // Wrapping add keeps ids nonzero-ish and cheap; 0 is reserved by the
        // proto as "don't care" but we never rely on that as a client.
        let id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
        if id == 0 {
            self.next_request_id.fetch_add(1, Ordering::Relaxed)
        } else {
            id
        }
    }

    fn write_frame(&mut self, envelope: &Envelope) -> Result<()> {
        let payload = envelope.encode_to_vec();
        let len = u32::try_from(payload.len()).unwrap_or(u32::MAX);
        if len > MAX_FRAME_LEN {
            return Err(IpcError::FrameTooLarge(len));
        }
        self.stream.write_all(&len.to_be_bytes())?;
        self.stream.write_all(&payload)?;
        Ok(())
    }

    fn read_frame(&mut self) -> Result<Envelope> {
        let mut len_buf = [0u8; 4];
        self.stream
            .read_exact(&mut len_buf)
            .map_err(map_eof(IpcError::ConnectionClosed))?;
        let len = u32::from_be_bytes(len_buf);
        if len > MAX_FRAME_LEN {
            return Err(IpcError::FrameTooLarge(len));
        }
        let mut payload = vec![0u8; len as usize];
        self.stream
            .read_exact(&mut payload)
            .map_err(map_eof(IpcError::ConnectionClosed))?;
        let envelope = Envelope::decode(payload.as_slice())?;
        Ok(envelope)
    }

    fn roundtrip(&mut self, request_id: u32, body: Body) -> Result<Envelope> {
        let request = Envelope {
            request_id,
            body: Some(body),
        };
        self.write_frame(&request)?;
        let response = self.read_frame()?;
        if response.request_id != request_id {
            return Err(IpcError::RequestIdMismatch {
                expected: request_id,
                got: response.request_id,
            });
        }
        Ok(response)
    }

    /// Send a `PingRequest` and return the echoed nonce.
    pub fn ping(&mut self, nonce: u32) -> Result<u32> {
        let request_id = self.fresh_request_id();
        let response = self.roundtrip(request_id, Body::PingRequest(PingRequest { nonce }))?;
        match response.body {
            Some(Body::PingResponse(PingResponse { nonce })) => Ok(nonce),
            _ => Err(IpcError::UnexpectedBody),
        }
    }

    /// Ask the subagent to write a config value it owns (docs/design.md 2.5).
    pub fn write_config(&mut self, key: &str, value: IpcValue) -> Result<WriteConfigResponse> {
        let request_id = self.fresh_request_id();
        let body = Body::WriteConfigRequest(WriteConfigRequest {
            key: key.to_string(),
            value: Some(value.into_pb()),
        });
        let response = self.roundtrip(request_id, body)?;
        match response.body {
            Some(Body::WriteConfigResponse(r)) => Ok(r),
            _ => Err(IpcError::UnexpectedBody),
        }
    }

    /// Read back a config value through the subagent.
    pub fn read_config(&mut self, key: &str) -> Result<ReadConfigResponse> {
        let request_id = self.fresh_request_id();
        let body = Body::ReadConfigRequest(ReadConfigRequest {
            key: key.to_string(),
        });
        let response = self.roundtrip(request_id, body)?;
        match response.body {
            Some(Body::ReadConfigResponse(r)) => Ok(r),
            _ => Err(IpcError::UnexpectedBody),
        }
    }

    /// Ask the subagent to emit a trap on the application's behalf.
    pub fn send_trap(&mut self, trap_name: &str) -> Result<SendTrapResponse> {
        let request_id = self.fresh_request_id();
        let body = Body::SendTrapRequest(SendTrapRequest {
            trap_name: trap_name.to_string(),
        });
        let response = self.roundtrip(request_id, body)?;
        match response.body {
            Some(Body::SendTrapResponse(r)) => Ok(r),
            _ => Err(IpcError::UnexpectedBody),
        }
    }
}

/// Maps an `UnexpectedEof` (or any I/O error on a zero-length read) to a
/// dedicated "closed" error rather than letting `read_exact`'s generic
/// `UnexpectedEof` propagate unlabeled.
fn map_eof(closed: IpcError) -> impl FnOnce(std::io::Error) -> IpcError {
    move |e| {
        if e.kind() == std::io::ErrorKind::UnexpectedEof {
            closed
        } else {
            IpcError::Io(e)
        }
    }
}

/// A value to send over IPC; a thin wrapper so callers of this crate don't
/// need to depend on the generated `pb` types directly.
#[derive(Debug, Clone, PartialEq)]
pub enum IpcValue {
    Int32(i32),
    Uint32(u32),
    Uint64(u64),
    Bytes(Vec<u8>),
}

impl IpcValue {
    fn into_pb(self) -> PbValue {
        match self {
            IpcValue::Int32(v) => PbValue {
                r#type: ValueType::Int32 as i32,
                int32_val: v,
                ..Default::default()
            },
            IpcValue::Uint32(v) => PbValue {
                r#type: ValueType::Uint32 as i32,
                uint32_val: v,
                ..Default::default()
            },
            IpcValue::Uint64(v) => PbValue {
                r#type: ValueType::Uint64 as i32,
                uint64_val: v,
                ..Default::default()
            },
            IpcValue::Bytes(v) => PbValue {
                r#type: ValueType::Bytes as i32,
                bytes_val: v,
                ..Default::default()
            },
        }
    }
}
