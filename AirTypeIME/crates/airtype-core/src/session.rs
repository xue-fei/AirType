//! 单次 PTT 会话状态机：帧 -> 语音段。
//! 只做协议与缓冲，不碰 ASR、不碰系统 API。

use thiserror::Error;

use crate::audio::{AudioError, PcmBuffer};
use crate::denoise::FrameDenoiser;
use crate::config::AudioConfig;
use crate::protocol::{Frame, FrameKind, StartMeta, CODEC_PCM_S16LE};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionState {
    Idle,
    Recording,
}

#[derive(Debug, Error)]
pub enum SessionError {
    #[error("AUDIO frame received before START")]
    NotStarted,
    #[error(transparent)]
    Audio(#[from] AudioError),
    #[error("invalid START metadata: {0}")]
    BadStart(String),
    #[error("codec `{0}` not supported by v1 (expected {CODEC_PCM_S16LE})")]
    UnsupportedCodec(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SessionEvent {
    Started {
        device: String,
        sample_rate: u32,
        declared_rate: u32,
        declared_channels: u16,
        declared_bits: u16,
    },
    Appended {
        samples: usize,
        total_ms: u64,
    },
    Utterance {
        samples: Vec<i16>,
        sample_rate: u32,
    },
    Cancelled,
    Ping,
    Ignored,
}

pub struct Session {
    state: SessionState,
    buffer: PcmBuffer,
    sample_rate: u32,
    max_ms: u32,
    last_seq: Option<u16>,
    seq_warnings: u64,
    /// 可选降噪器。装配在音频入口处，ASR 与音量表都基于增强后的信号。
    denoiser: Option<Box<dyn FrameDenoiser>>,
}

impl Session {
    pub fn new(cfg: &AudioConfig) -> Self {
        Self {
            state: SessionState::Idle,
            buffer: PcmBuffer::new(cfg.sample_rate, cfg.max_utterance_ms),
            sample_rate: cfg.sample_rate,
            max_ms: cfg.max_utterance_ms,
            last_seq: None,
            seq_warnings: 0,
            denoiser: None,
        }
    }

    /// 注入降噪器（daemon 按 `[denoise] enabled` 决定是否调用）。
    pub fn set_denoiser(&mut self, d: Box<dyn FrameDenoiser>) {
        self.denoiser = Some(d);
    }

    pub fn state(&self) -> SessionState {
        self.state
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    pub fn seq_warnings(&self) -> u64 {
        self.seq_warnings
    }

    pub fn on_frame(&mut self, f: &Frame) -> Result<SessionEvent, SessionError> {
        if let Some(prev) = self.last_seq {
            if f.seq != prev.wrapping_add(1) {
                self.seq_warnings += 1;
            }
        }
        self.last_seq = Some(f.seq);

        match f.kind {
            FrameKind::Start => {
                let meta: StartMeta = if f.payload.is_empty() {
                    StartMeta::default()
                } else {
                    serde_json::from_slice(&f.payload)
                        .map_err(|e| SessionError::BadStart(e.to_string()))?
                };
                if meta.codec != CODEC_PCM_S16LE {
                    return Err(SessionError::UnsupportedCodec(meta.codec));
                }
                self.state = SessionState::Recording;
                self.buffer.clear();
                self.last_seq = Some(f.seq);
                self.seq_warnings = 0;
                Ok(SessionEvent::Started {
                    device: meta.device,
                    sample_rate: self.sample_rate,
                    declared_rate: meta.sample_rate,
                    declared_channels: meta.channels,
                    declared_bits: meta.bits_per_sample,
                })
            }
            FrameKind::Audio => {
                if self.state != SessionState::Recording {
                    return Err(SessionError::NotStarted);
                }
                // 降噪放在入库前：后面 ASR 和音量表看到的都是增强后的信号。
                let appended = match self.denoiser.as_mut() {
                    Some(d) => match le_bytes_to_i16(&f.payload) {
                        Ok(samples) => {
                            let enhanced = d.process(&samples);
                            self.buffer.append_samples(&enhanced, self.max_ms)
                        }
                        Err(e) => Err(e),
                    },
                    None => self.buffer.append_le_bytes(&f.payload, self.max_ms),
                };
                match appended {
                    Ok(n) => Ok(SessionEvent::Appended {
                        samples: n,
                        total_ms: self.buffer.duration_ms(),
                    }),
                    Err(e @ AudioError::UtteranceTooLong(_)) => {
                        self.state = SessionState::Idle;
                        self.buffer.clear();
                        Err(SessionError::Audio(e))
                    }
                    Err(e) => Err(SessionError::Audio(e)),
                }
            }
            FrameKind::End => {
                if self.state != SessionState::Recording {
                    return Ok(SessionEvent::Ignored);
                }
                self.state = SessionState::Idle;
                // 降噪器可能还压着首窗填充/尾部样本，冲出来再交付，
                // 否则末尾几个字会缺一截（约 10~40ms）。
                if let Some(d) = self.denoiser.as_mut() {
                    let tail = d.flush();
                    if !tail.is_empty() {
                        if let Err(e) = self.buffer.append_samples(&tail, self.max_ms) {
                            // 超长就丢掉这点尾巴，不能因为余量让整段失败
                            tracing::warn!("降噪尾部追加失败，丢弃 {} 样本: {e}", tail.len());
                        }
                    }
                }
                Ok(SessionEvent::Utterance {
                    samples: self.buffer.take(),
                    sample_rate: self.sample_rate,
                })
            }
            FrameKind::Cancel => {
                self.state = SessionState::Idle;
                self.buffer.clear();
                Ok(SessionEvent::Cancelled)
            }
            FrameKind::Ping => Ok(SessionEvent::Ping),
            _ => Ok(SessionEvent::Ignored),
        }
    }
}

/// S16LE 字节流转 i16 样本。奇数字节属于协议错误，交给上层报。
fn le_bytes_to_i16(bytes: &[u8]) -> Result<Vec<i16>, AudioError> {
    if bytes.len() % 2 != 0 {
        return Err(AudioError::OddPayload(bytes.len()));
    }
    Ok(bytes
        .chunks_exact(2)
        .map(|p| i16::from_le_bytes([p[0], p[1]]))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg() -> AudioConfig {
        AudioConfig::default()
    }

    fn pcm_frame(seq: u16, n_samples: usize) -> Frame {
        Frame::new(FrameKind::Audio, seq, vec![0u8; n_samples * 2]).unwrap()
    }

    #[test]
    fn full_ptt_flow() {
        let mut s = Session::new(&cfg());
        let start = Frame::new(FrameKind::Start, 0, br#"{"device":"airmic-01"}"#.to_vec()).unwrap();
        assert!(matches!(
            s.on_frame(&start).unwrap(),
            SessionEvent::Started { device, .. } if device == "airmic-01"
        ));
        assert_eq!(s.state(), SessionState::Recording);
        s.on_frame(&pcm_frame(1, 320)).unwrap();
        s.on_frame(&pcm_frame(2, 320)).unwrap();
        match s.on_frame(&Frame::empty(FrameKind::End, 3)).unwrap() {
            SessionEvent::Utterance { samples, sample_rate } => {
                assert_eq!(samples.len(), 640);
                assert_eq!(sample_rate, 16000);
            }
            other => panic!("unexpected: {other:?}"),
        }
        assert_eq!(s.state(), SessionState::Idle);
    }

    #[test]
    fn audio_before_start_is_rejected() {
        let mut s = Session::new(&cfg());
        assert!(matches!(
            s.on_frame(&pcm_frame(0, 10)),
            Err(SessionError::NotStarted)
        ));
    }

    #[test]
    fn cancel_drops_buffer() {
        let mut s = Session::new(&cfg());
        s.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        s.on_frame(&pcm_frame(1, 320)).unwrap();
        assert!(matches!(
            s.on_frame(&Frame::empty(FrameKind::Cancel, 2)).unwrap(),
            SessionEvent::Cancelled
        ));
        match s.on_frame(&Frame::empty(FrameKind::End, 3)).unwrap() {
            SessionEvent::Ignored => {}
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn rejects_non_pcm_codec() {
        let mut s = Session::new(&cfg());
        let start = Frame::new(FrameKind::Start, 0, br#"{"codec":"opus"}"#.to_vec()).unwrap();
        assert!(matches!(
            s.on_frame(&start),
            Err(SessionError::UnsupportedCodec(_))
        ));
    }

    #[test]
    fn seq_gap_is_counted() {
        let mut s = Session::new(&cfg());
        s.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        s.on_frame(&pcm_frame(1, 10)).unwrap();
        s.on_frame(&pcm_frame(9, 10)).unwrap();
        assert_eq!(s.seq_warnings(), 1);
    }
}
