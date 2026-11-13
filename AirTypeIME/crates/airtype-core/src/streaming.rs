//! 流式 ASR 适配层：`start / push / finish`，边说边出字。
//!
//! 与整段解码（`asr::AsrEngine`）是并列的两条路径，由 `[streaming] enabled`
//! 决定走哪条。这里只定义 trait 与不依赖 native 库的 mock 实现；
//! 生产用的 sherpa-onnx 流式引擎在 `airtype-asr-sherpa`，由 daemon 装配。

use std::sync::Arc;

use crate::asr::{AsrError};
use crate::config::AsrConfig;

/// 一次增量解码的结果。`text` 是**当前累积**识别文本（不是增量片段），
/// 调用方自己算最长公共前缀来决定要撤销多少、追加多少。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StreamOutcome {
    pub text: String,
    pub is_final: bool,
}

pub trait StreamingAsr: Send + Sync {
    fn name(&self) -> &str;
    fn start(&self) -> Result<Box<dyn AsrStream>, AsrError>;
}

pub trait AsrStream: Send {
    /// 喂一段增量 PCM，返回当前累积文本。
    fn push(&mut self, samples: &[i16], sample_rate: u32) -> Result<StreamOutcome, AsrError>;
    /// 收尾：喂完最后一段音频并取回最终文本。
    fn finish(&mut self) -> Result<StreamOutcome, AsrError>;
    /// 取消：丢弃本段，服务端要释放会话。
    fn cancel(&mut self);
}

/// 按配置构造流式引擎。sherpa 引擎不在这里——core 不能依赖它的 native 库，
/// 由 `airtype-daemon` 按 `asr.engine` 分发。
pub fn build_streaming_engine(cfg: &AsrConfig) -> Result<Arc<dyn StreamingAsr>, AsrError> {
    match cfg.engine.as_str() {
        "mock" | "" => Ok(Arc::new(MockStreamingAsr {
            text: cfg.mock_text.clone(),
        })),
        "command" => Err(AsrError::Config(
            "streaming is not supported by the `command` engine (use mock or sherpa)".into(),
        )),
        "sherpa" => Err(AsrError::Config(
            "sherpa 流式引擎由 airtype-daemon 装配（core 不能依赖 sherpa-onnx）".into(),
        )),
        other => Err(AsrError::Config(format!(
            "unknown asr engine `{other}` (supported: mock; sherpa 由 daemon 装配)"
        ))),
    }
}

// ---------------------------------------------------------------------------
// Mock：按累计音频时长吐出递增文本，用来在没有 ASR 引擎时验证流式时序与注入。
// ---------------------------------------------------------------------------

pub struct MockStreamingAsr {
    pub text: String,
}

impl StreamingAsr for MockStreamingAsr {
    fn name(&self) -> &str {
        "mock-streaming"
    }
    fn start(&self) -> Result<Box<dyn AsrStream>, AsrError> {
        Ok(Box::new(MockStream {
            text: self.text.clone(),
            samples: 0,
        }))
    }
}

struct MockStream {
    text: String,
    samples: usize,
}

impl AsrStream for MockStream {
    fn push(&mut self, samples: &[i16], sample_rate: u32) -> Result<StreamOutcome, AsrError> {
        self.samples += samples.len();
        let ms = self.samples as u64 * 1000 / sample_rate.max(1) as u64;
        // 每 100ms 多吐 3 个字，模拟真实模型逐步出字。
        let n = (ms as usize / 100) * 3;
        let text: String = self.text.chars().take(n).collect();
        Ok(StreamOutcome {
            text,
            is_final: false,
        })
    }
    fn finish(&mut self) -> Result<StreamOutcome, AsrError> {
        Ok(StreamOutcome {
            text: self.text.clone(),
            is_final: true,
        })
    }
    fn cancel(&mut self) {}
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_stream_reveals_text_progressively() {
        let factory = MockStreamingAsr {
            text: "你好世界".into(),
        };
        let mut s = factory.start().unwrap();
        // 每次 push 1600 samples = 100ms @16kHz
        let out = s.push(&vec![0i16; 1600], 16000).unwrap();
        assert_eq!(out.text, "你好世");
        let out = s.push(&vec![0i16; 1600], 16000).unwrap();
        assert_eq!(out.text, "你好世界");
        let out = s.finish().unwrap();
        assert_eq!(out.text, "你好世界");
        assert!(out.is_final);
    }

    #[test]
    fn command_engine_is_rejected_for_streaming() {
        let mut cfg = AsrConfig::default();
        cfg.engine = "command".into();
        assert!(build_streaming_engine(&cfg).is_err());
    }

    #[test]
    fn sherpa_is_assembled_by_daemon_not_core() {
        let mut cfg = AsrConfig::default();
        cfg.engine = "sherpa".into();
        let err = match build_streaming_engine(&cfg) {
            Ok(_) => panic!("core 不应能装配 sherpa 流式引擎"),
            Err(e) => e,
        };
        assert!(
            err.to_string().contains("daemon"),
            "错误信息应指明由 daemon 装配: {err}"
        );
    }
}
