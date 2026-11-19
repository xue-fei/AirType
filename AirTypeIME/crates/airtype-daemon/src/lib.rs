//! # airtype-daemon
//!
//! 平台相关的编排层：WebSocket 服务、会话调度、文本注入。
//! 业务逻辑全部委托给 `airtype-core`，这里只负责 I/O 与系统能力。

pub mod engines;
pub mod inject;
pub mod registry;
pub mod server;
pub mod web;

use std::sync::Arc;

pub use engines::build as build_engines;
pub use inject::{build_injector, TextInjector};
pub use registry::{DeviceStatus, Registry};
pub use server::run_server;
pub use web::run_web;

/// 识别完成后的文本通知。FFI 外壳注册的回调会被包装成这个闭包。
pub type TextCallback = Arc<dyn Fn(String) + Send + Sync>;

/// 流式识别中的中间结果通知（每次 partial 都会触发一次）。
///
/// - `text`：当前累积文本，即「如果此刻落定，输入框里应该是什么」
/// - `delete_chars`：相对上一次上屏文本需要退格的字符数（Unicode 字符，非字节）
/// - `append`：在退格之后追加的增量文本
///
/// 外壳可以只用 `text` 做整体替换，也可以用 `delete_chars`/`append` 做最小编辑。
pub type PartialCallback = Arc<dyn Fn(String, usize, String) + Send + Sync>;
