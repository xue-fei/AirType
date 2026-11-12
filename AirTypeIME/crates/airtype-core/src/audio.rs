//! PCM 缓冲与 WAV 封装。第一版只处理 16kHz / 16bit / 单声道 / S16LE。
//! 若后续引入 Opus / ADPCM，只需在这里加一层 codec 解码，状态机与 ASR 不动。

use thiserror::Error;

#[derive(Debug, Error)]
pub enum AudioError {
    #[error("AUDIO payload has odd length {0}, expected S16LE samples")]
    OddPayload(usize),
    #[error("AUDIO payload {0} bytes exceeds per-frame limit")]
    FrameTooLarge(usize),
    #[error("utterance exceeds max duration {0} ms")]
    UtteranceTooLong(u32),
}

/// 一次 PTT 语音段的采样点累积缓冲。
pub struct PcmBuffer {
    samples: Vec<i16>,
    sample_rate: u32,
    max_samples: usize,
}

impl PcmBuffer {
    pub fn new(sample_rate: u32, max_utterance_ms: u32) -> Self {
        let max_samples = (sample_rate as u64 * max_utterance_ms as u64 / 1000) as usize;
        Self {
            samples: Vec::new(),
            sample_rate,
            max_samples,
        }
    }

    pub fn clear(&mut self) {
        self.samples.clear();
    }

    pub fn append_le_bytes(&mut self, bytes: &[u8], max_ms: u32) -> Result<usize, AudioError> {
        if bytes.len() % 2 != 0 {
            return Err(AudioError::OddPayload(bytes.len()));
        }
        if bytes.len() > super::protocol::MAX_PAYLOAD {
            return Err(AudioError::FrameTooLarge(bytes.len()));
        }
        let incoming = bytes.len() / 2;
        if self.samples.len() + incoming > self.max_samples {
            return Err(AudioError::UtteranceTooLong(max_ms));
        }
        for pair in bytes.chunks_exact(2) {
            self.samples.push(i16::from_le_bytes([pair[0], pair[1]]));
        }
        Ok(incoming)
    }

    pub fn samples(&self) -> &[i16] {
        &self.samples
    }

    pub fn is_empty(&self) -> bool {
        self.samples.is_empty()
    }

    pub fn len(&self) -> usize {
        self.samples.len()
    }

    pub fn duration_ms(&self) -> u64 {
        self.samples.len() as u64 * 1000 / self.sample_rate as u64
    }

    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    /// 取出缓冲并清空，用于 `END` 之后交给 ASR。
    pub fn take(&mut self) -> Vec<i16> {
        std::mem::take(&mut self.samples)
    }
}

/// 生成标准 RIFF/WAVE（PCM, 16bit, mono）字节流，供命令式 ASR 引擎读取。
pub fn encode_wav(samples: &[i16], sample_rate: u32) -> Vec<u8> {
    let data_len = (samples.len() * 2) as u32;
    let mut out = Vec::with_capacity(44 + data_len as usize);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36u32.wrapping_add(data_len)).to_le_bytes());
    out.extend_from_slice(b"WAVE");
    out.extend_from_slice(b"fmt ");
    out.extend_from_slice(&16u32.to_le_bytes()); // fmt chunk size
    out.extend_from_slice(&1u16.to_le_bytes()); // audio format = PCM
    out.extend_from_slice(&1u16.to_le_bytes()); // channels = mono
    out.extend_from_slice(&sample_rate.to_le_bytes());
    out.extend_from_slice(&(sample_rate * 2).to_le_bytes()); // byte rate
    out.extend_from_slice(&2u16.to_le_bytes()); // block align
    out.extend_from_slice(&16u16.to_le_bytes()); // bits per sample
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_len.to_le_bytes());
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn appends_s16le_and_tracks_duration() {
        let mut b = PcmBuffer::new(16000, 60_000);
        let bytes = [0x01, 0x00, 0xFF, 0xFF, 0x00, 0x01];
        assert_eq!(b.append_le_bytes(&bytes, 60_000).unwrap(), 3);
        assert_eq!(b.samples(), &[1, -1, 256]);
        assert_eq!(b.len(), 3);
        assert_eq!(b.take().len(), 3);
        assert!(b.is_empty());
    }

    #[test]
    fn rejects_odd_payload_and_overflow() {
        let mut b = PcmBuffer::new(16000, 60_000);
        assert!(matches!(
            b.append_le_bytes(&[1, 2, 3], 60_000),
            Err(AudioError::OddPayload(3))
        ));
        let mut tiny = PcmBuffer::new(16000, 1); // max 16 samples
        assert!(matches!(
            tiny.append_le_bytes(&[0u8; 40], 1),
            Err(AudioError::UtteranceTooLong(1))
        ));
    }

    #[test]
    fn wav_header_is_valid() {
        let wav = encode_wav(&[0, 1, -1], 16000);
        assert_eq!(&wav[0..4], b"RIFF");
        assert_eq!(&wav[8..12], b"WAVE");
        assert_eq!(&wav[12..16], b"fmt ");
        assert_eq!(u32::from_le_bytes(wav[24..28].try_into().unwrap()), 16000);
        assert_eq!(u32::from_le_bytes(wav[40..44].try_into().unwrap()), 6);
        assert_eq!(wav.len(), 50);
    }
}
