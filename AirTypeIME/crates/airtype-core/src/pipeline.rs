//! 把会话状态机、ASR、文本后处理串成一条同步流水线。
//! 同步设计是有意的：核心库不依赖任何异步运行时，daemon 用
//! `spawn_blocking` 调它，FFI 外壳也能直接调，macOS/Linux 同样可复用。
//!
//! 两条并列路径，由 `[streaming] enabled` 决定：
//!
//! - 整段（默认）：`END` 后一次解码，产出 `Recognized(text)`。
//! - 流式：`START` 开流，`AUDIO` 按 chunk 增量解码产出 `Partial`，
//!   `END` 收尾产出 `Final`。两个事件都带 `delete_chars` / `append`，
//!   上层用「撤销重写」把文字实时铺进输入框。

use std::sync::Arc;
use std::time::Instant;

use thiserror::Error;

use crate::asr::{AsrEngine, AsrError};
use crate::audio::{i16_to_le_bytes, le_bytes_to_i16, AudioError};
use crate::config::Config;
use crate::postprocess::PostProcessor;
use crate::protocol::{Frame, FrameKind, Timings, MAX_PAYLOAD};
use crate::session::{Session, SessionError, SessionEvent};
use crate::streaming::{AsrStream, StreamingAsr};

#[derive(Debug, Error)]
pub enum PipelineError {
    #[error(transparent)]
    Session(#[from] SessionError),
    #[error("asr: {0}")]
    Asr(#[from] AsrError),
    #[error(transparent)]
    Audio(#[from] AudioError),
    #[error("streaming: {0}")]
    Streaming(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PipelineEvent {
    Started {
        device: String,
        sample_rate: u32,
    },
    Appended {
        samples: usize,
        total_ms: u64,
    },
    /// 整段解码（`streaming.enabled = false`）的最终文本。
    Recognized(String),
    /// 流式中间结果。`text` 是当前累积文本；`delete_chars` 是本次需要退格
    /// 撤销的字符数；`append` 是要追加的增量文本。上层调 `inject_replace`。
    Partial {
        text: String,
        delete_chars: usize,
        append: String,
    },
    /// 流式最终文本，字段含义同 `Partial`。
    Final {
        text: String,
        delete_chars: usize,
        append: String,
    },
    Cancelled,
    /// 流式取消：`delete_chars` 为已上屏、需要撤销的字符数。
    CancelledStreaming {
        delete_chars: usize,
    },
    Ping,
    /// END 但无有效音频，或 ASR/后处理后为空
    Empty,
    Ignored,
}

pub struct Pipeline {
    session: Session,
    asr: Arc<dyn AsrEngine>,
    post: PostProcessor,
    /// `None` = 整段模式；`Some` = 流式模式。
    streaming: Option<Arc<dyn StreamingAsr>>,
    stream: Option<Box<dyn AsrStream>>,
    chunk_samples: usize,
    /// 攒够 `chunk_samples` 才向 ASR 服务请求一次增量识别。
    pending: Vec<i16>,
    /// 已上屏文本，用于计算「撤销多少、追加多少」。
    displayed: String,
    replace: bool,
    last_timing: Option<Timings>,
    stream_asr_ms: u64,
    /// 可选降噪器。**必须挂在这里而不是 Session 里**：流式识别直接吃
    /// `frame.payload`，只在 Session 降噪等于对 ASR 完全无效（实测六档 SNR
    /// 开/关降噪结果逐字相同，就是这个原因）。
    denoiser: Option<Box<dyn crate::denoise::FrameDenoiser>>,
}

impl Pipeline {
    /// 注入降噪器（builder 风格，不改 `new` 的签名以免波及所有调用点）。
    ///
    /// 必须在收到第一帧之前调用：降噪状态（RNN/OLA）是按流累积的。
    pub fn with_denoiser(mut self, d: Box<dyn crate::denoise::FrameDenoiser>) -> Self {
        self.denoiser = Some(d);
        self
    }

    /// `streaming` 由上层（daemon / FFI 外壳）按 `asr.engine` 装配后传入：
    /// core 定义 trait，但不能依赖 sherpa-onnx 之类的 native 引擎实现。
    pub fn new(
        cfg: &Config,
        asr: Arc<dyn AsrEngine>,
        streaming: Option<Arc<dyn StreamingAsr>>,
    ) -> Result<Self, PipelineError> {
        Ok(Self {
            session: Session::new(&cfg.audio),
            asr,
            post: PostProcessor::new(cfg.postprocess.clone()),
            streaming,
            stream: None,
            chunk_samples: (cfg.audio.sample_rate as u64 * cfg.streaming.chunk_ms.max(1) as u64
                / 1000) as usize,
            pending: Vec::new(),
            displayed: String::new(),
            replace: cfg.streaming.replace,
            last_timing: None,
            stream_asr_ms: 0,
            denoiser: None,
        })
    }

    pub fn asr_name(&self) -> &str {
        self.asr.name()
    }

    pub fn streaming(&self) -> bool {
        self.streaming.is_some()
    }

    pub fn seq_warnings(&self) -> u64 {
        self.session.seq_warnings()
    }

    /// 最近一次识别的分阶段耗时。daemon 会在同一次 `spawn_blocking`
    /// 里取走它，补上注入耗时后写进 `STATUS.timings`。
    pub fn last_timing(&self) -> Option<Timings> {
        self.last_timing.clone()
    }

    pub fn on_frame(&mut self, f: &Frame) -> Result<PipelineEvent, PipelineError> {
        self.last_timing = None;
        // 降噪必须在分支之前做：流式识别直接用 frame.payload 喂 ASR，
        // 只降噪 Session 那条路等于「离线路径安静了、ASR 一点没变」。
        // 增强后的帧同时供 Session（离线/电平累计）与流式两条路使用。
        let enhanced = if f.kind == FrameKind::Audio {
            match self.denoiser.as_mut() {
                Some(d) => match le_bytes_to_i16(&f.payload) {
                    Ok(samples) => {
                        let out = d.process(&samples);
                        Some(Frame {
                            kind: f.kind,
                            flags: f.flags,
                            seq: f.seq,
                            payload: i16_to_le_bytes(&out),
                        })
                    }
                    // 奇数字节复用 Session 的错误路径，这里保持原帧即可。
                    Err(_) => None,
                },
                None => None,
            }
        } else {
            None
        };
        let f = enhanced.as_ref().unwrap_or(f);
        let ev = self.session.on_frame(f)?;
        if self.streaming.is_some() {
            self.on_frame_streaming(f, ev)
        } else {
            self.on_frame_utterance(ev)
        }
    }

    fn on_frame_utterance(&mut self, ev: SessionEvent) -> Result<PipelineEvent, PipelineError> {
        Ok(match ev {
            SessionEvent::Started {
                device,
                sample_rate,
                ..
            } => PipelineEvent::Started {
                device,
                sample_rate,
            },
            SessionEvent::Appended { samples, total_ms } => PipelineEvent::Appended {
                samples,
                total_ms,
            },
            SessionEvent::Utterance { samples, .. } => {
                if samples.is_empty() {
                    return Ok(PipelineEvent::Empty);
                }
                let audio_ms = samples.len() as u64 * 1000 / self.session.sample_rate() as u64;
                let t0 = Instant::now();
                let raw = self.asr.transcribe(&samples, self.session.sample_rate())?;
                let asr_ms = t0.elapsed().as_millis() as u64;
                let t1 = Instant::now();
                let text = self.post.process(&raw);
                let post_ms = t1.elapsed().as_millis() as u64;
                self.last_timing = Some(Timings {
                    asr_ms,
                    post_ms,
                    audio_ms,
                    ..Timings::default()
                });
                if text.is_empty() {
                    PipelineEvent::Empty
                } else {
                    PipelineEvent::Recognized(text)
                }
            }
            SessionEvent::Cancelled => PipelineEvent::Cancelled,
            SessionEvent::Ping => PipelineEvent::Ping,
            SessionEvent::Ignored => PipelineEvent::Ignored,
        })
    }

    fn on_frame_streaming(
        &mut self,
        f: &Frame,
        ev: SessionEvent,
    ) -> Result<PipelineEvent, PipelineError> {
        match ev {
            SessionEvent::Started {
                device,
                sample_rate,
                ..
            } => {
                let factory = self
                    .streaming
                    .as_ref()
                    .ok_or_else(|| PipelineError::Streaming("streaming engine missing".into()))?;
                let t0 = Instant::now();
                let stream = factory.start()?;
                self.stream_asr_ms += t0.elapsed().as_millis() as u64;
                self.stream = Some(stream);
                self.pending.clear();
                self.displayed.clear();
                Ok(PipelineEvent::Started {
                    device,
                    sample_rate,
                })
            }
            SessionEvent::Appended { samples, total_ms } => {
                let chunk = decode_audio_payload(&f.payload)?;
                self.pending.extend_from_slice(&chunk);
                if self.pending.len() >= self.chunk_samples {
                    return self.flush_partial();
                }
                Ok(PipelineEvent::Appended { samples, total_ms })
            }
            SessionEvent::Utterance { samples, .. } => {
                let audio_ms = samples.len() as u64 * 1000 / self.session.sample_rate() as u64;
                let t0 = Instant::now();
                let mut final_text = String::new();
                if let Some(stream) = self.stream.as_mut() {
                    if !self.pending.is_empty() {
                        let chunk = std::mem::take(&mut self.pending);
                        // push 只为让模型看到尾音，其 partial 结果会被 finish 覆盖。
                        let _ = stream.push(&chunk, self.session.sample_rate());
                    }
                    if let Ok(outcome) = stream.finish() {
                        final_text = outcome.text;
                    }
                }
                self.stream_asr_ms += t0.elapsed().as_millis() as u64;
                self.stream = None;

                let t1 = Instant::now();
                let processed = self.post.process(&final_text);
                let post_ms = t1.elapsed().as_millis() as u64;
                // force=true：即使 `replace=false`（partial 阶段不上屏），
                // 收尾也要一次性把全文写进去。
                let (delete_chars, append) = self.diff(processed.clone(), true);
                self.last_timing = Some(Timings {
                    asr_ms: self.stream_asr_ms,
                    post_ms,
                    audio_ms,
                    ..Timings::default()
                });
                self.stream_asr_ms = 0;

                if processed.is_empty() && append.is_empty() {
                    Ok(PipelineEvent::Empty)
                } else {
                    Ok(PipelineEvent::Final {
                        text: processed,
                        delete_chars,
                        append,
                    })
                }
            }
            SessionEvent::Cancelled => {
                if let Some(mut stream) = self.stream.take() {
                    stream.cancel();
                }
                self.pending.clear();
                let delete_chars = if self.replace {
                    self.displayed.chars().count()
                } else {
                    0
                };
                self.displayed.clear();
                Ok(PipelineEvent::CancelledStreaming { delete_chars })
            }
            SessionEvent::Ping => Ok(PipelineEvent::Ping),
            SessionEvent::Ignored => Ok(PipelineEvent::Ignored),
        }
    }

    /// 把攒够的一小段 PCM 交给流式引擎，算出需要撤销/追加的文本。
    fn flush_partial(&mut self) -> Result<PipelineEvent, PipelineError> {
        let chunk = std::mem::take(&mut self.pending);
        let t0 = Instant::now();
        let raw = match self.stream.as_mut() {
            Some(stream) => stream.push(&chunk, self.session.sample_rate())?.text,
            None => String::new(),
        };
        self.stream_asr_ms += t0.elapsed().as_millis() as u64;
        let processed = self.post.process_partial(&raw);
        let (delete_chars, append) = self.diff(processed, false);
        if delete_chars == 0 && append.is_empty() {
            return Ok(PipelineEvent::Ignored);
        }
        Ok(PipelineEvent::Partial {
            text: self.displayed.clone(),
            delete_chars,
            append,
        })
    }

    /// 计算「已上屏文本」到「新文本」的差异：返回 (退格字符数, 追加文本)。
    /// `replace = false` 时 partial 阶段不上屏（返回 0/空），但收尾用
    /// `force = true` 一次性把全文写进去。
    fn diff(&mut self, new_text: String, force: bool) -> (usize, String) {
        if !self.replace && !force {
            return (0, String::new());
        }
        let old: Vec<char> = self.displayed.chars().collect();
        let new: Vec<char> = new_text.chars().collect();
        let mut common = 0;
        while common < old.len() && common < new.len() && old[common] == new[common] {
            common += 1;
        }
        let delete_chars = old.len() - common;
        let append: String = new[common..].iter().collect();
        self.displayed = new_text;
        (delete_chars, append)
    }
}

fn decode_audio_payload(bytes: &[u8]) -> Result<Vec<i16>, PipelineError> {
    if bytes.len() % 2 != 0 {
        return Err(AudioError::OddPayload(bytes.len()).into());
    }
    if bytes.len() > MAX_PAYLOAD {
        return Err(AudioError::FrameTooLarge(bytes.len()).into());
    }
    Ok(bytes
        .chunks_exact(2)
        .map(|p| i16::from_le_bytes([p[0], p[1]]))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::asr::build_engine;
    use crate::protocol::FrameKind;
    use crate::streaming::{build_streaming_engine, StreamingAsr};

    fn cfg() -> Config {
        let mut c = Config::default();
        // 本模块的用例都在验证「整段解码」路径（END 后一次识别出 Recognized）。
        // streaming 默认已改为开启，这里显式关掉；流式路径由 streaming.rs
        // 和下面的 streaming_* 用例覆盖。
        c.streaming.enabled = false;
        c
    }

    /// 模拟 daemon 的装配：按 `streaming.enabled` 决定是否给流式引擎。
    fn streaming_for(c: &Config) -> Option<Arc<dyn StreamingAsr>> {
        if c.streaming.enabled {
            Some(build_streaming_engine(&c.asr).unwrap())
        } else {
            None
        }
    }

    fn pcm_frame(seq: u16, n_samples: usize) -> Frame {
        Frame::new(FrameKind::Audio, seq, vec![0u8; n_samples * 2]).unwrap()
    }

    #[test]
    fn end_to_end_from_frames_to_final_text() {
        let c = cfg();
        let asr = build_engine(&c.asr).unwrap();
        let mut p = Pipeline::new(&c, asr, streaming_for(&c)).unwrap();
        assert!(!p.streaming());

        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        p.on_frame(&pcm_frame(1, 320)).unwrap();
        let ev = p.on_frame(&Frame::empty(FrameKind::End, 2)).unwrap();
        assert_eq!(
            ev,
            PipelineEvent::Recognized("你好，这是一次 AirType 语音输入测试。".into())
        );
    }

    #[test]
    fn end_without_audio_yields_empty() {
        let c = cfg();
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        assert_eq!(
            p.on_frame(&Frame::empty(FrameKind::End, 1)).unwrap(),
            PipelineEvent::Empty
        );
    }

    #[test]
    fn protocol_errors_surface_as_pipeline_errors() {
        let c = cfg();
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        let err = p
            .on_frame(&Frame::new(FrameKind::Audio, 0, vec![0u8; 4]).unwrap())
            .unwrap_err();
        assert!(matches!(err, PipelineError::Session(SessionError::NotStarted)));
    }

    #[test]
    fn utterance_records_stage_timings() {
        let c = cfg();
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        let t0 = p.on_frame(&pcm_frame(1, 320)).unwrap();
        assert!(matches!(t0, PipelineEvent::Appended { .. }));
        assert!(p.last_timing().is_none(), "AUDIO 帧不应产生识别耗时");

        p.on_frame(&Frame::empty(FrameKind::End, 2)).unwrap();
        let t = p.last_timing().expect("END 后应有耗时记录");
        assert_eq!(t.audio_ms, 20); // 320 samples @16kHz
        assert_eq!(t.inject_ms, 0, "注入耗时由 daemon 补");
        assert_eq!(t.total_ms, 0, "总耗时由 daemon 补");
    }

    // -----------------------------------------------------------------------
    // 流式
    // -----------------------------------------------------------------------

    fn streaming_cfg(chunk_ms: u32, replace: bool) -> Config {
        let mut c = Config::default();
        c.streaming.enabled = true;
        c.streaming.chunk_ms = chunk_ms;
        c.streaming.replace = replace;
        c
    }

    #[test]
    fn streaming_emits_progressive_partials_then_final() {
        let c = streaming_cfg(100, true);
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        assert!(p.streaming());

        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        // 每帧 320 samples = 20ms；chunk 100ms => 第 5 帧触发一次 partial
        let mut partials = Vec::new();
        for i in 1..=8u16 {
            match p.on_frame(&pcm_frame(i, 320)).unwrap() {
                PipelineEvent::Partial { text, delete_chars, append } => {
                    partials.push((text, delete_chars, append));
                }
                _ => {}
            }
        }
        assert!(!partials.is_empty(), "应有中间结果");
        // 第一次 partial 一定是纯追加（displayed 为空）
        assert_eq!(partials[0].1, 0);
        assert!(!partials[0].2.is_empty());

        let fin = p.on_frame(&Frame::empty(FrameKind::End, 9)).unwrap();
        match fin {
            PipelineEvent::Final { text, delete_chars, append } => {
                assert_eq!(text, "你好，这是一次 AirType 语音输入测试。");
                // 末尾一次性补齐标点，至少会追加 "。"
                assert!(append.ends_with('。'));
                assert!(delete_chars <= text.chars().count());
            }
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn streaming_without_replace_only_writes_on_final() {
        let c = streaming_cfg(100, false);
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        for i in 1..=8u16 {
            let ev = p.on_frame(&pcm_frame(i, 320)).unwrap();
            assert!(
                !matches!(ev, PipelineEvent::Partial { .. }),
                "replace=false 时不应产生 partial 注入: {ev:?}"
            );
        }
        match p.on_frame(&Frame::empty(FrameKind::End, 9)).unwrap() {
            PipelineEvent::Final { text, delete_chars, append } => {
                assert_eq!(text, "你好，这是一次 AirType 语音输入测试。");
                assert_eq!(delete_chars, 0);
                assert_eq!(append, text, "一次性注入完整文本");
            }
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn streaming_cancel_reports_chars_to_undo() {
        let c = streaming_cfg(100, true);
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        for i in 1..=8u16 {
            p.on_frame(&pcm_frame(i, 320)).unwrap();
        }
        match p.on_frame(&Frame::empty(FrameKind::Cancel, 9)).unwrap() {
            PipelineEvent::CancelledStreaming { delete_chars } => {
                assert!(delete_chars > 0, "应报告已上屏字符数以便撤销");
            }
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn streaming_odd_audio_payload_is_rejected() {
        let c = streaming_cfg(100, true);
        let mut p = Pipeline::new(&c, build_engine(&c.asr).unwrap(), streaming_for(&c)).unwrap();
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        let err = p
            .on_frame(&Frame::new(FrameKind::Audio, 1, vec![0u8, 0u8, 0u8]).unwrap())
            .unwrap_err();
        assert!(matches!(err, PipelineError::Session(SessionError::Audio(_))));
    }
}
