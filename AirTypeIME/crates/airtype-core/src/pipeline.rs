//! 把会话状态机、ASR、文本后处理串成一条同步流水线。
//! 同步设计是有意的：核心库不依赖任何异步运行时，daemon 用
//! `spawn_blocking` 调它，FFI 外壳也能直接调，macOS/Linux 同样可复用。

use std::sync::Arc;
use thiserror::Error;

use crate::asr::{AsrEngine, AsrError};
use crate::config::Config;
use crate::postprocess::PostProcessor;
use crate::protocol::Frame;
use crate::session::{Session, SessionError, SessionEvent};

#[derive(Debug, Error)]
pub enum PipelineError {
    #[error(transparent)]
    Session(#[from] SessionError),
    #[error("asr: {0}")]
    Asr(#[from] AsrError),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PipelineEvent {
    Started { device: String, sample_rate: u32 },
    Appended { samples: usize, total_ms: u64 },
    /// 已完成 ASR + 后处理、即将注入的最终文本
    Recognized(String),
    Cancelled,
    Ping,
    /// END 但无有效音频，或 ASR/后处理后为空
    Empty,
    Ignored,
}

pub struct Pipeline {
    session: Session,
    asr: Arc<dyn AsrEngine>,
    post: PostProcessor,
}

impl Pipeline {
    pub fn new(cfg: &Config, asr: Arc<dyn AsrEngine>) -> Self {
        Self {
            session: Session::new(&cfg.audio),
            asr,
            post: PostProcessor::new(cfg.postprocess.clone()),
        }
    }

    pub fn asr_name(&self) -> &str {
        self.asr.name()
    }

    pub fn seq_warnings(&self) -> u64 {
        self.session.seq_warnings()
    }

    pub fn on_frame(&mut self, f: &Frame) -> Result<PipelineEvent, PipelineError> {
        let ev = self.session.on_frame(f)?;
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
                let raw = self.asr.transcribe(&samples, self.session.sample_rate())?;
                let text = self.post.process(&raw);
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::asr::build_engine;
    use crate::protocol::FrameKind;

    #[test]
    fn end_to_end_from_frames_to_final_text() {
        let cfg = Config::default();
        let asr = build_engine(&cfg.asr).unwrap();
        let mut p = Pipeline::new(&cfg, asr);

        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        p.on_frame(&Frame::new(FrameKind::Audio, 1, vec![0u8; 640]).unwrap())
            .unwrap();
        let ev = p
            .on_frame(&Frame::empty(FrameKind::End, 2))
            .unwrap();
        assert_eq!(
            ev,
            PipelineEvent::Recognized("你好，这是一次 AirType 语音输入测试。".into())
        );
    }

    #[test]
    fn end_without_audio_yields_empty() {
        let cfg = Config::default();
        let mut p = Pipeline::new(&cfg, build_engine(&cfg.asr).unwrap());
        p.on_frame(&Frame::empty(FrameKind::Start, 0)).unwrap();
        assert_eq!(
            p.on_frame(&Frame::empty(FrameKind::End, 1)).unwrap(),
            PipelineEvent::Empty
        );
    }

    #[test]
    fn protocol_errors_surface_as_pipeline_errors() {
        let cfg = Config::default();
        let mut p = Pipeline::new(&cfg, build_engine(&cfg.asr).unwrap());
        let err = p
            .on_frame(&Frame::new(FrameKind::Audio, 0, vec![0u8; 4]).unwrap())
            .unwrap_err();
        assert!(matches!(err, PipelineError::Session(SessionError::NotStarted)));
    }
}
