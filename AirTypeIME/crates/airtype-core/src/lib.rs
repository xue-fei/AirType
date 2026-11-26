//! # airtype-core
//!
//! AirTypeIME 的平台无关共享核心。刻意保持 **纯同步、零异步运行时、零平台 API**：
//! 既能被 `airtype-daemon` 在 `spawn_blocking` 中调用，也能被未来的
//! TSF / InputMethodKit / Fcitx5 原生外壳通过 C ABI 直接调用。
//!
//! ```text
//! WebSocket binary 帧
//!   -> protocol::Frame      编解码
//!   -> session::Session     PTT 状态机 + PCM 缓冲
//!   -> asr::AsrEngine       可切换的识别引擎
//!   -> postprocess          标点 / 数字 / 口头词
//!   -> PipelineEvent::Recognized(String)  => 交给外壳注入
//! ```

pub mod asr;
pub mod audio;
pub mod config;
pub mod denoise;
pub mod pipeline;
pub mod postprocess;
pub mod protocol;
pub mod session;
pub mod streaming;

pub use asr::{build_engine, AsrEngine, MockAsr};
pub use config::Config;
pub use pipeline::{Pipeline, PipelineEvent, PipelineError};
pub use protocol::{Frame, FrameKind, StartMeta, StatusPayload, Timings};
pub use session::{Session, SessionEvent, SessionState};
pub use streaming::{build_streaming_engine, AsrStream, StreamOutcome, StreamingAsr};
