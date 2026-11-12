use std::path::PathBuf;
use std::sync::Arc;

use airtype_core::Config;
use airtype_daemon::{run_server, TextCallback};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info"));
    tracing_subscriber::fmt().with_env_filter(filter).init();

    let config_path = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("config/airtype.toml"));

    let config = if config_path.exists() {
        Config::load(&config_path)
            .with_context_read(&config_path)?
    } else {
        tracing::warn!(
            "配置文件 {} 不存在，使用内置默认值（可传路径作为第一个参数）",
            config_path.display()
        );
        Config::default()
    };

    let on_text: TextCallback = Arc::new(|text: String| {
        tracing::info!(target: "airtype", "[FINAL TEXT] {text}");
    });

    let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
    tokio::spawn(async move {
        if tokio::signal::ctrl_c().await.is_ok() {
            let _ = stop_tx.send(());
        }
    });

    println!(
        "AirTypeIME v{} | ws://{}:{}{} | ASR={} | inject={}",
        env!("CARGO_PKG_VERSION"),
        config.server.bind,
        config.server.port,
        config.server.path,
        config.asr.engine,
        config.inject.mode
    );
    println!("按 Ctrl+C 退出。用 tools/mock_airmic.py 模拟 AirMic 推流。");

    run_server(config, on_text, stop_rx).await
}

/// 给 anyhow 的 Context 加一条带路径的说明，避免污染主流程。
trait WithContextRead<T> {
    fn with_context_read(self, path: &std::path::Path) -> anyhow::Result<T>;
}

impl<T, E> WithContextRead<T> for Result<T, E>
where
    E: std::error::Error + Send + Sync + 'static,
{
    fn with_context_read(self, path: &std::path::Path) -> anyhow::Result<T> {
        use anyhow::Context;
        self.with_context(|| format!("读取配置 {} 失败", path.display()))
    }
}
