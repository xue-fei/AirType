//! AirMic Node <-> AirTypeIME 帧协议 v1，见 `docs/PROTOCOL.md`。
//!
//! 8 字节头（Little Endian）：magic / version / kind / flags / seq:u16 / payload_len:u16
//! 业务帧一律承载在 WebSocket binary 帧里。

use serde::{Deserialize, Serialize};
use thiserror::Error;

pub const MAGIC: u8 = 0xA1;
pub const VERSION: u8 = 0x01;
pub const HEADER_LEN: usize = 8;
pub const MAX_PAYLOAD: usize = 4096;
pub const CODEC_PCM_S16LE: &str = "pcm_s16le";

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameKind {
    Start = 0x01,
    Audio = 0x02,
    End = 0x03,
    Cancel = 0x04,
    Ping = 0x05,
    Pong = 0x06,
    Status = 0x81,
    Text = 0x82,
}

impl FrameKind {
    pub fn from_u8(v: u8) -> Option<Self> {
        Some(match v {
            0x01 => Self::Start,
            0x02 => Self::Audio,
            0x03 => Self::End,
            0x04 => Self::Cancel,
            0x05 => Self::Ping,
            0x06 => Self::Pong,
            0x81 => Self::Status,
            0x82 => Self::Text,
            _ => return None,
        })
    }
}

#[derive(Debug, Error)]
pub enum ProtocolError {
    #[error("frame too short: {0} bytes (need at least {HEADER_LEN})")]
    TooShort(usize),
    #[error("bad magic 0x{0:02X}, expected 0xA1")]
    BadMagic(u8),
    #[error("unsupported protocol version {0}")]
    BadVersion(u8),
    #[error("unknown frame kind 0x{0:02X}")]
    UnknownKind(u8),
    #[error("payload length mismatch: header says {0}, actually {1}")]
    LengthMismatch(usize, usize),
    #[error("payload too large: {0} bytes (max {MAX_PAYLOAD})")]
    PayloadTooLarge(usize),
}

/// `START` 帧的 JSON 元数据。所有字段都有默认值，硬件端可以只发 `{}`。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StartMeta {
    #[serde(default = "default_device")]
    pub device: String,
    #[serde(default = "default_codec")]
    pub codec: String,
    #[serde(default = "default_sample_rate")]
    pub sample_rate: u32,
    #[serde(default = "default_channels")]
    pub channels: u16,
    #[serde(default = "default_bits")]
    pub bits_per_sample: u16,
}

impl Default for StartMeta {
    fn default() -> Self {
        Self {
            device: default_device(),
            codec: default_codec(),
            sample_rate: default_sample_rate(),
            channels: default_channels(),
            bits_per_sample: default_bits(),
        }
    }
}

fn default_device() -> String {
    "unknown".into()
}
fn default_codec() -> String {
    CODEC_PCM_S16LE.into()
}
fn default_sample_rate() -> u32 {
    16000
}
fn default_channels() -> u16 {
    1
}
fn default_bits() -> u16 {
    16
}

/// 一次识别的各阶段耗时（毫秒），用于定位 0.3~1s 延迟目标的瓶颈。
/// `asr/post/audio` 由 core 填，`inject/total` 由 daemon 填，
/// `first_partial` 由 daemon 在流式模式下填（START 到首个 `Partial`）。
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq, Eq)]
pub struct Timings {
    #[serde(default)]
    pub asr_ms: u64,
    #[serde(default)]
    pub post_ms: u64,
    #[serde(default)]
    pub audio_ms: u64,
    #[serde(default)]
    pub inject_ms: u64,
    #[serde(default)]
    pub total_ms: u64,
    #[serde(default)]
    pub first_partial_ms: u64,
}

/// 服务端 `STATUS` 帧的 JSON payload。
/// `timings` 是 v1.1 追加的可选字段：旧客户端只取 `state`，不受影响。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StatusPayload {
    pub state: String,
    #[serde(default)]
    pub text: String,
    #[serde(default)]
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timings: Option<Timings>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Frame {
    pub kind: FrameKind,
    pub flags: u8,
    pub seq: u16,
    pub payload: Vec<u8>,
}

impl Frame {
    pub fn new(kind: FrameKind, seq: u16, payload: impl Into<Vec<u8>>) -> Result<Self, ProtocolError> {
        let payload = payload.into();
        if payload.len() > MAX_PAYLOAD {
            return Err(ProtocolError::PayloadTooLarge(payload.len()));
        }
        Ok(Self {
            kind,
            flags: 0,
            seq,
            payload,
        })
    }

    pub fn empty(kind: FrameKind, seq: u16) -> Self {
        Self {
            kind,
            flags: 0,
            seq,
            payload: Vec::new(),
        }
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(HEADER_LEN + self.payload.len());
        out.push(MAGIC);
        out.push(VERSION);
        out.push(self.kind as u8);
        out.push(self.flags);
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&(self.payload.len() as u16).to_le_bytes());
        out.extend_from_slice(&self.payload);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < HEADER_LEN {
            return Err(ProtocolError::TooShort(bytes.len()));
        }
        if bytes[0] != MAGIC {
            return Err(ProtocolError::BadMagic(bytes[0]));
        }
        if bytes[1] != VERSION {
            return Err(ProtocolError::BadVersion(bytes[1]));
        }
        let kind = FrameKind::from_u8(bytes[2]).ok_or(ProtocolError::UnknownKind(bytes[2]))?;
        let seq = u16::from_le_bytes([bytes[4], bytes[5]]);
        let len = u16::from_le_bytes([bytes[6], bytes[7]]) as usize;
        if len > MAX_PAYLOAD {
            return Err(ProtocolError::PayloadTooLarge(len));
        }
        let actual = bytes.len() - HEADER_LEN;
        if actual != len {
            return Err(ProtocolError::LengthMismatch(len, actual));
        }
        Ok(Self {
            kind,
            flags: bytes[3],
            seq,
            payload: bytes[HEADER_LEN..].to_vec(),
        })
    }

    pub fn payload_str(&self) -> Option<&str> {
        std::str::from_utf8(&self.payload).ok()
    }

    pub fn start_meta(&self) -> Result<StartMeta, serde_json::Error> {
        if self.payload.is_empty() {
            return Ok(StartMeta::default());
        }
        serde_json::from_slice(&self.payload)
    }

    pub fn status(seq: u16, state: &str, text: &str, message: &str) -> Self {
        Self::status_with_timings(seq, state, text, message, None)
    }

    pub fn status_with_timings(
        seq: u16,
        state: &str,
        text: &str,
        message: &str,
        timings: Option<Timings>,
    ) -> Self {
        let payload = serde_json::to_vec(&StatusPayload {
            state: state.to_owned(),
            text: text.to_owned(),
            message: message.to_owned(),
            timings,
        })
        .unwrap_or_default();
        Self {
            kind: FrameKind::Status,
            flags: 0,
            seq,
            payload,
        }
    }

    pub fn text(seq: u16, utf8: &str) -> Self {
        Self {
            kind: FrameKind::Text,
            flags: 0,
            seq,
            payload: utf8.as_bytes().to_vec(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_all_kinds() {
        for kind in [
            FrameKind::Start,
            FrameKind::Audio,
            FrameKind::End,
            FrameKind::Cancel,
            FrameKind::Ping,
            FrameKind::Pong,
            FrameKind::Status,
            FrameKind::Text,
        ] {
            let f = Frame::new(kind, 42, vec![1u8, 2, 3, 4]).unwrap();
            assert_eq!(Frame::decode(&f.encode()).unwrap(), f);
        }
    }

    #[test]
    fn header_layout_is_stable() {
        let f = Frame::new(FrameKind::Audio, 0x0102, vec![0x10, 0x20]).unwrap();
        assert_eq!(
            f.encode(),
            vec![0xA1, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02, 0x00, 0x10, 0x20]
        );
    }

    #[test]
    fn rejects_corrupt_frames() {
        assert!(matches!(
            Frame::decode(&[0xA1, 0x01]),
            Err(ProtocolError::TooShort(2))
        ));
        let mut bad_magic = Frame::empty(FrameKind::Ping, 0).encode();
        bad_magic[0] = 0x00;
        assert!(Frame::decode(&bad_magic).is_err());
        let mut bad_len = Frame::empty(FrameKind::Ping, 0).encode();
        bad_len[6] = 7;
        assert!(matches!(
            Frame::decode(&bad_len),
            Err(ProtocolError::LengthMismatch(7, 0))
        ));
    }

    #[test]
    fn start_meta_defaults_to_pcm16k() {
        let meta: StartMeta = serde_json::from_str("{}").unwrap();
        assert_eq!(meta.codec, CODEC_PCM_S16LE);
        assert_eq!(meta.sample_rate, 16000);
        assert_eq!(meta.channels, 1);
        assert_eq!(meta.bits_per_sample, 16);
    }

    #[test]
    fn status_timings_are_optional_and_backward_compatible() {
        let plain = Frame::status(1, "done", "你好", "");
        let parsed: StatusPayload = serde_json::from_slice(&plain.payload).unwrap();
        assert_eq!(parsed.state, "done");
        assert!(parsed.timings.is_none());
        // v1 客户端发来的旧 JSON 没有 timings 字段，仍要能反序列化。
        let old: StatusPayload =
            serde_json::from_str(r#"{"state":"done","text":"x","message":""}"#).unwrap();
        assert!(old.timings.is_none());

        let timed = Frame::status_with_timings(
            2,
            "done",
            "你好",
            "",
            Some(Timings {
                asr_ms: 120,
                post_ms: 1,
                audio_ms: 1500,
                inject_ms: 80,
                total_ms: 210,
                first_partial_ms: 95,
            }),
        );
        let parsed: StatusPayload = serde_json::from_slice(&timed.payload).unwrap();
        let t = parsed.timings.unwrap();
        assert_eq!(t.asr_ms, 120);
        assert_eq!(t.total_ms, 210);
        assert_eq!(t.audio_ms, 1500);
        assert_eq!(t.first_partial_ms, 95);
    }
}
