//! Locks in the exact byte layout produced by `Value::encode`, so any drift
//! from the C `storage_lmdb.h` encoding is caught immediately. See that
//! header's "Value encoding" comment: 1-byte tag, 3 reserved zero bytes,
//! little-endian payload.

use agentx_rust_app::storage::{CodecError, StorageType, Value};

#[test]
fn int32_negative_one() {
    assert_eq!(
        Value::Int32(-1).encode(),
        vec![0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff]
    );
}

#[test]
fn int32_positive() {
    assert_eq!(
        Value::Int32(0x0102_0304).encode(),
        vec![0x01, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01]
    );
}

#[test]
fn uint32_value() {
    assert_eq!(
        Value::Uint32(0xDEAD_BEEF).encode(),
        vec![0x02, 0x00, 0x00, 0x00, 0xEF, 0xBE, 0xAD, 0xDE]
    );
}

#[test]
fn uint64_value() {
    assert_eq!(
        Value::Uint64(0x0102_0304_0506_0708).encode(),
        vec![0x03, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01]
    );
}

#[test]
fn bytes_value() {
    assert_eq!(
        Value::Bytes(vec![0xAA, 0xBB, 0xCC]).encode(),
        vec![0x04, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC]
    );
}

#[test]
fn bytes_empty() {
    assert_eq!(Value::Bytes(vec![]).encode(), vec![0x04, 0x00, 0x00, 0x00]);
}

#[test]
fn oid_value() {
    // sysUpTime-ish OID: 1.3.6.1.2.1.1.3
    let oid = vec![1u32, 3, 6, 1, 2, 1, 1, 3];
    let encoded = Value::Oid(oid.clone()).encode();
    assert_eq!(encoded[0], 0x05);
    assert_eq!(&encoded[1..4], &[0, 0, 0]);
    assert_eq!(encoded.len(), 4 + oid.len() * 4);
    // first subid little-endian
    assert_eq!(&encoded[4..8], &[1, 0, 0, 0]);
}

#[test]
fn decode_is_inverse_of_encode() {
    let values = vec![
        Value::Int32(i32::MIN),
        Value::Int32(-1),
        Value::Int32(0),
        Value::Int32(i32::MAX),
        Value::Uint32(0),
        Value::Uint32(u32::MAX),
        Value::Uint64(0),
        Value::Uint64(u64::MAX),
        Value::Bytes(vec![]),
        Value::Bytes(vec![1, 2, 3, 4, 5]),
        Value::Oid(vec![]),
        Value::Oid(vec![1, 3, 6, 1]),
    ];
    for v in values {
        let encoded = v.encode();
        let decoded = Value::decode(&encoded).expect("decode should succeed");
        assert_eq!(decoded, v);
    }
}

#[test]
fn decode_rejects_short_buffer() {
    for len in 0..4 {
        let buf = vec![1u8; len];
        assert_eq!(Value::decode(&buf), Err(CodecError::BufferTooShort { len }));
    }
}

#[test]
fn decode_rejects_unknown_tag() {
    assert_eq!(Value::decode(&[0, 0, 0, 0]), Err(CodecError::UnknownTag(0)));
    assert_eq!(
        Value::decode(&[200, 0, 0, 0, 1, 2, 3, 4]),
        Err(CodecError::UnknownTag(200))
    );
}

#[test]
fn decode_rejects_nonzero_reserved_bytes() {
    assert_eq!(
        Value::decode(&[1, 1, 0, 0, 1, 2, 3, 4]),
        Err(CodecError::ReservedNonZero([1, 0, 0]))
    );
}

#[test]
fn decode_rejects_mismatched_length() {
    assert_eq!(
        Value::decode(&[1, 0, 0, 0, 1, 2, 3]),
        Err(CodecError::InvalidPayloadLength {
            tag: StorageType::Int32,
            len: 3
        })
    );
    assert_eq!(
        Value::decode(&[2, 0, 0, 0, 1, 2, 3, 4, 5]),
        Err(CodecError::InvalidPayloadLength {
            tag: StorageType::Uint32,
            len: 5
        })
    );
    assert_eq!(
        Value::decode(&[3, 0, 0, 0, 1, 2, 3, 4]),
        Err(CodecError::InvalidPayloadLength {
            tag: StorageType::Uint64,
            len: 4
        })
    );
    // OID payload not a multiple of 4.
    assert_eq!(
        Value::decode(&[5, 0, 0, 0, 1, 2, 3, 4, 5]),
        Err(CodecError::InvalidPayloadLength {
            tag: StorageType::Oid,
            len: 5
        })
    );
}
