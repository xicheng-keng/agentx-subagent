//! Byte-for-byte compatible value codec with `include/storage_lmdb.h`.
//!
//! Layout (see the header's "Value encoding" doc comment):
//!
//! ```text
//! offset 0      : u8  type tag
//! offset 1..3   : u8  reserved, always 0
//! offset 4..    : little-endian payload
//! ```

use thiserror::Error;

/// The `storage_type_t` tag values from `storage_lmdb.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum StorageType {
    Int32 = 1,
    Uint32 = 2,
    Uint64 = 3,
    Bytes = 4,
    Oid = 5,
}

impl StorageType {
    fn from_tag(tag: u8) -> Option<Self> {
        match tag {
            1 => Some(Self::Int32),
            2 => Some(Self::Uint32),
            3 => Some(Self::Uint64),
            4 => Some(Self::Bytes),
            5 => Some(Self::Oid),
            _ => None,
        }
    }
}

/// Number of header bytes preceding the payload (tag + 3 reserved bytes).
pub const HEADER_LEN: usize = 4;

/// A typed value as stored in either LMDB environment.
///
/// Variants correspond exactly to the `storage_type_t` tags in
/// `include/storage_lmdb.h`; `STORAGE_TYPE_INVALID` (tag 0) has no variant
/// here because it is never a valid on-disk value.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Value {
    Int32(i32),
    Uint32(u32),
    Uint64(u64),
    Bytes(Vec<u8>),
    /// A MIB OID: a sequence of sub-identifiers, each a `u32`.
    Oid(Vec<u32>),
}

/// Errors that can occur while decoding a stored value.
#[derive(Debug, Error, PartialEq, Eq)]
pub enum CodecError {
    #[error("buffer too short: got {len} bytes, need at least {HEADER_LEN}")]
    BufferTooShort { len: usize },
    #[error("reserved header bytes must be zero, got {0:?}")]
    ReservedNonZero([u8; 3]),
    #[error("unknown storage type tag {0}")]
    UnknownTag(u8),
    #[error("payload length {len} is invalid for tag {tag:?}")]
    InvalidPayloadLength { tag: StorageType, len: usize },
}

impl Value {
    /// The `storage_type_t` tag for this value.
    pub fn storage_type(&self) -> StorageType {
        match self {
            Value::Int32(_) => StorageType::Int32,
            Value::Uint32(_) => StorageType::Uint32,
            Value::Uint64(_) => StorageType::Uint64,
            Value::Bytes(_) => StorageType::Bytes,
            Value::Oid(_) => StorageType::Oid,
        }
    }

    /// Encode into the on-disk representation described in `storage_lmdb.h`.
    pub fn encode(&self) -> Vec<u8> {
        let tag = self.storage_type() as u8;
        let mut buf = Vec::with_capacity(HEADER_LEN + self.payload_len_hint());
        buf.push(tag);
        buf.extend_from_slice(&[0u8, 0u8, 0u8]);
        match self {
            Value::Int32(v) => buf.extend_from_slice(&v.to_le_bytes()),
            Value::Uint32(v) => buf.extend_from_slice(&v.to_le_bytes()),
            Value::Uint64(v) => buf.extend_from_slice(&v.to_le_bytes()),
            Value::Bytes(v) => buf.extend_from_slice(v),
            Value::Oid(subids) => {
                for s in subids {
                    buf.extend_from_slice(&s.to_le_bytes());
                }
            }
        }
        buf
    }

    fn payload_len_hint(&self) -> usize {
        match self {
            Value::Int32(_) | Value::Uint32(_) => 4,
            Value::Uint64(_) => 8,
            Value::Bytes(v) => v.len(),
            Value::Oid(v) => v.len() * 4,
        }
    }

    /// Decode from the on-disk representation, rejecting anything that does
    /// not match `storage_lmdb.h` byte for byte.
    pub fn decode(buf: &[u8]) -> Result<Value, CodecError> {
        if buf.len() < HEADER_LEN {
            return Err(CodecError::BufferTooShort { len: buf.len() });
        }
        let tag = buf[0];
        let reserved = [buf[1], buf[2], buf[3]];
        if reserved != [0, 0, 0] {
            return Err(CodecError::ReservedNonZero(reserved));
        }
        let ty = StorageType::from_tag(tag).ok_or(CodecError::UnknownTag(tag))?;
        let payload = &buf[HEADER_LEN..];
        match ty {
            StorageType::Int32 => {
                let arr: [u8; 4] = payload.try_into().map_err(|_| {
                    CodecError::InvalidPayloadLength { tag: ty, len: payload.len() }
                })?;
                Ok(Value::Int32(i32::from_le_bytes(arr)))
            }
            StorageType::Uint32 => {
                let arr: [u8; 4] = payload.try_into().map_err(|_| {
                    CodecError::InvalidPayloadLength { tag: ty, len: payload.len() }
                })?;
                Ok(Value::Uint32(u32::from_le_bytes(arr)))
            }
            StorageType::Uint64 => {
                let arr: [u8; 8] = payload.try_into().map_err(|_| {
                    CodecError::InvalidPayloadLength { tag: ty, len: payload.len() }
                })?;
                Ok(Value::Uint64(u64::from_le_bytes(arr)))
            }
            StorageType::Bytes => Ok(Value::Bytes(payload.to_vec())),
            StorageType::Oid => {
                if payload.len() % 4 != 0 {
                    return Err(CodecError::InvalidPayloadLength { tag: ty, len: payload.len() });
                }
                let subids = payload
                    .chunks_exact(4)
                    .map(|c| u32::from_le_bytes(c.try_into().expect("chunk is 4 bytes")))
                    .collect();
                Ok(Value::Oid(subids))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn int32_negative_one_matches_c_layout() {
        assert_eq!(Value::Int32(-1).encode(), vec![0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff]);
    }

    #[test]
    fn short_buffer_rejected() {
        assert_eq!(Value::decode(&[1, 0, 0]), Err(CodecError::BufferTooShort { len: 3 }));
    }

    #[test]
    fn unknown_tag_rejected() {
        assert_eq!(
            Value::decode(&[9, 0, 0, 0, 1, 2, 3, 4]),
            Err(CodecError::UnknownTag(9))
        );
    }

    #[test]
    fn mismatched_length_rejected() {
        assert_eq!(
            Value::decode(&[1, 0, 0, 0, 1, 2, 3]),
            Err(CodecError::InvalidPayloadLength { tag: StorageType::Int32, len: 3 })
        );
    }
}
