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

    /// 直接追加已解码的样本（降噪后的数据走这条路，避免再转回字节）。
    pub fn append_samples(&mut self, samples: &[i16], max_ms: u32) -> Result<usize, AudioError> {
        if self.samples.len() + samples.len() > self.max_samples {
            return Err(AudioError::UtteranceTooLong(max_ms));
        }
        self.samples.extend_from_slice(samples);
        Ok(samples.len())
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

/// 一帧 S16LE PCM 的归一化电平，单位「毫电平」：0 = 静音，1000 = 满幅。
///
/// 先算 RMS，再转 dBFS 做线性映射：**-60 dB 记 0、0 dB 记 1000**。
/// 为什么不用线性 RMS：正常说话大约 -30 dBFS（RMS≈0.03），线性映射只能占到
/// 3% 高度，图标几乎不动；对数映射后同一句话能到 500，视觉上才跟得上音量。
/// 全程静音时 RMS 为 0，`max(1e-6)` 把 -120 dB 的 log 兜住，结果为 0。
///
/// 返回整数而非 f32：`PipelineEvent` 派生了 `Eq`（f32 不实现 Eq），
/// 且跨 ABI 传给 C++ 时整数比浮点更省心。
/// S16LE 字节流转 i16 样本。奇数字节属于协议错误。
pub fn le_bytes_to_i16(bytes: &[u8]) -> Result<Vec<i16>, AudioError> {
    if bytes.len() % 2 != 0 {
        return Err(AudioError::OddPayload(bytes.len()));
    }
    Ok(bytes
        .chunks_exact(2)
        .map(|p| i16::from_le_bytes([p[0], p[1]]))
        .collect())
}

/// i16 样本转回 S16LE 字节流（降噪后重新组帧用）。
pub fn i16_to_le_bytes(samples: &[i16]) -> Vec<u8> {
    let mut out = Vec::with_capacity(samples.len() * 2);
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

pub fn frame_level(bytes: &[u8]) -> u16 {
    let count = bytes.len() / 2;
    if count == 0 {
        return 0;
    }
    let mut sum = 0.0f64;
    for pair in bytes.chunks_exact(2) {
        let s = i16::from_le_bytes([pair[0], pair[1]]) as f64 / 32768.0;
        sum += s * s;
    }
    let rms = (sum / count as f64).sqrt() as f32;
    let db = 20.0 * rms.max(1e-6).log10();

    // 噪声门：麦克风底噪通常落在 -70~-50 dBFS，它本身不为 0，映射到
    // 0~1000 后仍会占几个百分点——表现为「没说话时图标底部一直留一
    // 条绿线」。低于 -48 dBFS 一律判静音。
    // 阈值取 -48 而不是 -60：正常说话约 -30 dBFS、轻声约 -40，
    // -48 能挡住底噪又不至于把轻声也切掉。
    const NOISE_FLOOR_DB: f32 = -48.0;
    if db < NOISE_FLOOR_DB {
        return 0;
    }

    let norm = ((db + 60.0) / 60.0).clamp(0.0, 1.0);
    (norm * 1000.0).round() as u16
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

    #[test]
    fn silent_and_noise_frames_hit_zero() {
        // 数字静音
        assert_eq!(frame_level(&[0u8; 640]), 0);
        // 极小幅度底噪（RMS 约 3e-5，-90dBFS）也必须判 0，
        // 否则图标底部会留一条绿线
        let noise: Vec<u8> = (0..320).flat_map(|_| 1i16.to_le_bytes()).collect();
        assert_eq!(frame_level(&noise), 0);
        // 正常说话（幅度 0.1，约 -20dBFS）要能过门，且不该满格
        let speech: Vec<u8> = (0..320).flat_map(|_| (-3277i16).to_le_bytes()).collect();
        let lv = frame_level(&speech);
        assert!(lv > 300 && lv < 1000, "speech level = {lv}");
    }
}
