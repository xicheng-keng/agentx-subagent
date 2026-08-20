//! Frames the IPC client against a hand-rolled mock server on a temporary
//! `UnixListener`, covering: a normal ping round trip, a frame split across
//! multiple writes, an oversized length prefix, and a server that closes the
//! connection mid-response.

use agentx_rust_app::ipc::pb as agentx_ipc_pb;
use agentx_rust_app::ipc::{IpcClient, MAX_FRAME_LEN};
use prost::Message;
use std::io::{Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;
use std::thread;
use std::time::Duration;

fn socket_path(name: &str) -> PathBuf {
    let mut p = std::env::temp_dir();
    p.push(format!(
        "agentx-ipc-test-{name}-{}.sock",
        std::process::id()
    ));
    let _ = std::fs::remove_file(&p);
    p
}

fn read_frame(stream: &mut UnixStream) -> Option<Vec<u8>> {
    let mut len_buf = [0u8; 4];
    stream.read_exact(&mut len_buf).ok()?;
    let len = u32::from_be_bytes(len_buf) as usize;
    let mut payload = vec![0u8; len];
    stream.read_exact(&mut payload).ok()?;
    Some(payload)
}

fn write_frame(stream: &mut UnixStream, payload: &[u8]) {
    let len = payload.len() as u32;
    stream.write_all(&len.to_be_bytes()).unwrap();
    stream.write_all(payload).unwrap();
}

#[test]
fn ping_round_trip() {
    let path = socket_path("ping");
    let listener = UnixListener::bind(&path).unwrap();

    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let req = read_frame(&mut stream).unwrap();
        let envelope = agentx_ipc_pb::Envelope::decode(req.as_slice()).unwrap();
        let request_id = envelope.request_id;
        let nonce = match envelope.body {
            Some(agentx_ipc_pb::envelope::Body::PingRequest(r)) => r.nonce,
            _ => panic!("expected ping request"),
        };
        let response = agentx_ipc_pb::Envelope {
            request_id,
            body: Some(agentx_ipc_pb::envelope::Body::PingResponse(
                agentx_ipc_pb::PingResponse { nonce },
            )),
        };
        write_frame(&mut stream, &response.encode_to_vec());
    });

    let mut client = IpcClient::connect(&path).unwrap();
    let nonce_to_ping: u32 = rand::random();
    let echoed = client.ping(nonce_to_ping).unwrap();
    assert_eq!(echoed, nonce_to_ping);

    server.join().unwrap();
    let _ = std::fs::remove_file(&path);
}

#[test]
fn split_frame_is_reassembled() {
    let path = socket_path("split");
    let listener = UnixListener::bind(&path).unwrap();

    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let req = read_frame(&mut stream).unwrap();
        let envelope = agentx_ipc_pb::Envelope::decode(req.as_slice()).unwrap();
        let request_id = envelope.request_id;
        let nonce = match envelope.body {
            Some(agentx_ipc_pb::envelope::Body::PingRequest(r)) => r.nonce,
            _ => panic!("expected ping request"),
        };
        let response = agentx_ipc_pb::Envelope {
            request_id,
            body: Some(agentx_ipc_pb::envelope::Body::PingResponse(
                agentx_ipc_pb::PingResponse { nonce },
            )),
        };
        let payload = response.encode_to_vec();
        let len = payload.len() as u32;
        // Write the frame in three deliberately awkward pieces: half the
        // length prefix, the rest of the prefix plus a byte of payload,
        // then the remainder -- with small delays so the client's
        // `read_exact` calls actually have to wait for more data.
        stream.write_all(&len.to_be_bytes()[..2]).unwrap();
        stream.flush().unwrap();
        thread::sleep(Duration::from_millis(20));
        stream.write_all(&len.to_be_bytes()[2..]).unwrap();
        stream.write_all(&payload[..1.min(payload.len())]).unwrap();
        stream.flush().unwrap();
        thread::sleep(Duration::from_millis(20));
        stream.write_all(&payload[1.min(payload.len())..]).unwrap();
    });

    let mut client = IpcClient::connect(&path).unwrap();
    let nonce_to_ping: u32 = rand::random();
    let echoed = client.ping(nonce_to_ping).unwrap();
    assert_eq!(echoed, nonce_to_ping);

    server.join().unwrap();
    let _ = std::fs::remove_file(&path);
}

#[test]
fn oversized_length_prefix_is_rejected() {
    let path = socket_path("oversize");
    let listener = UnixListener::bind(&path).unwrap();

    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        // Consume the client's request frame so the server side of the
        // handshake looks normal, then reply with an oversized length
        // prefix instead of a valid frame.
        let _ = read_frame(&mut stream);
        let bogus_len = MAX_FRAME_LEN + 1;
        stream.write_all(&bogus_len.to_be_bytes()).unwrap();
        // No payload follows; the client must reject based on the prefix
        // alone rather than trying to read that many bytes.
    });

    let mut client = IpcClient::connect(&path).unwrap();
    let err = client.ping(rand::random()).unwrap_err();
    assert!(
        matches!(err, agentx_rust_app::ipc::IpcError::FrameTooLarge(_)),
        "{err:?}"
    );

    server.join().unwrap();
    let _ = std::fs::remove_file(&path);
}

#[test]
fn server_closing_mid_response_is_a_clean_error() {
    let path = socket_path("closemid");
    let listener = UnixListener::bind(&path).unwrap();

    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let _ = read_frame(&mut stream);
        // Announce a frame, send a few bytes of it, then drop the
        // connection before the rest arrives.
        let len: u32 = 100;
        stream.write_all(&len.to_be_bytes()).unwrap();
        stream.write_all(&[1, 2, 3]).unwrap();
        stream.flush().unwrap();
        // Dropping `stream` here closes the socket.
    });

    let mut client = IpcClient::connect(&path).unwrap();
    let err = client.ping(rand::random()).unwrap_err();
    assert!(
        matches!(err, agentx_rust_app::ipc::IpcError::ConnectionClosed),
        "{err:?}"
    );

    server.join().unwrap();
    let _ = std::fs::remove_file(&path);
}
