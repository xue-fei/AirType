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
