//! WebSocket 服务端：每个 AirMic 连接独占一条 Pipeline。

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};

use anyhow::{anyhow, Context};
use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::oneshot;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;

use airtype_core::asr::AsrEngine;
use airtype_core::{build_engine, Config, Frame, FrameKind, Pipeline, PipelineEvent};

use crate::inject::{build_injector, TextInjector};
use crate::TextCallback;

type WsSink = futures_util::stream::SplitSink<WebSocketStream<TcpStream>, Message>;

struct ServerState {
    config: Config,
    asr: Arc<dyn AsrEngine>,
    injector: Arc<dyn TextInjector>,
    on_text: TextCallback,
}

/// 启动服务，直到 `shutdown` 被触发。非阻塞于调用者之外（本身是 async）。
pub async fn run_server(
    config: Config,
    on_text: TextCallback,
    mut shutdown: oneshot::Receiver<()>,
) -> anyhow::Result<()> {
    let requested: SocketAddr = format!("{}:{}", config.server.bind, config.server.port)
        .parse()
        .context("invalid server.bind / server.port")?;
    let listener = TcpListener::bind(&requested)
        .await
        .with_context(|| format!("bind {requested} failed"))?;
    let addr = listener.local_addr()?;

    let asr = build_engine(&config.asr).context("build ASR engine")?;
    let injector = build_injector(&config.inject).context("build text injector")?;

    tracing::info!(
        %addr,
        asr = asr.name(),
        inject = injector.name(),
        path = %config.server.path,
        "AirTypeIME WebSocket 服务已启动"
    );

    let state = Arc::new(ServerState {
        config,
        asr,
        injector,
        on_text,
    });

    loop {
        tokio::select! {
            _ = &mut shutdown => {
                tracing::info!("收到停止信号，关闭服务");
                break;
            }
            accepted = listener.accept() => {
                let (stream, peer) = accepted.context("accept failed")?;
                let st = state.clone();
                tokio::spawn(async move {
                    if let Err(e) = handle_connection(stream, peer, st).await {
                        tracing::warn!(%peer, error = %e, "连接异常结束");
                    }
                });
            }
        }
    }
    Ok(())
}

async fn handle_connection(
    stream: TcpStream,
    peer: SocketAddr,
    state: Arc<ServerState>,
) -> anyhow::Result<()> {
    let ws = tokio_tungstenite::accept_async(stream)
        .await
        .with_context(|| format!("websocket handshake with {peer} failed"))?;
    let (mut sink, mut source) = ws.split();

    let pipeline = Arc::new(Mutex::new(Pipeline::new(&state.config, state.asr.clone())));
    tracing::info!(%peer, "AirMic 已连接");
    send(&mut sink, Frame::status(0, "ready", "", "")).await?;

    while let Some(msg) = source.next().await {
        let msg = msg.context("websocket read error")?;
        match msg {
            Message::Binary(bytes) => {
                let frame = match Frame::decode(&bytes) {
                    Ok(f) => f,
                    Err(e) => {
                        tracing::warn!(%peer, error = %e, "丢弃非法帧");
                        send(&mut sink, Frame::status(0, "error", "", &e.to_string())).await?;
                        continue;
                    }
                };
                let seq = frame.seq;
                let p = pipeline.clone();
                let outcome = tokio::task::spawn_blocking(move || {
                    p.lock()
                        .map_err(|_| anyhow!("pipeline mutex poisoned"))?
                        .on_frame(&frame)
                        .map_err(anyhow::Error::from)
                })
                .await
                .context("pipeline worker panicked")?;

                match outcome {
                    Ok(event) => dispatch(event, &state, &mut sink, seq).await?,
                    Err(e) => {
                        tracing::warn!(%peer, error = %e, "帧处理失败");
                        send(&mut sink, Frame::status(seq, "error", "", &e.to_string())).await?;
                    }
                }
            }
            Message::Text(t) => tracing::debug!(%peer, "文本帧（v1 忽略）: {t}"),
            Message::Close(_) => break,
            _ => {}
        }
    }

    tracing::info!(%peer, seq_warnings = pipeline.lock().map(|p| p.seq_warnings()).unwrap_or(0), "AirMic 断开");
    Ok(())
}

async fn dispatch(
    event: PipelineEvent,
    state: &Arc<ServerState>,
    sink: &mut WsSink,
    seq: u16,
) -> anyhow::Result<()> {
    match event {
        PipelineEvent::Started { device, sample_rate } => {
            tracing::info!(%device, %sample_rate, "PTT 开始");
            send(sink, Frame::status(seq, "recognizing", "", "")).await?;
        }
        PipelineEvent::Appended { samples, total_ms } => {
            tracing::trace!(%samples, %total_ms, "追加 PCM");
        }
        PipelineEvent::Recognized(text) => {
            tracing::info!(target: "airtype", "最终文本: {text}");

            let injector = state.injector.clone();
            let t = text.clone();
            let injected = tokio::task::spawn_blocking(move || injector.inject(&t))
                .await
                .context("injector worker panicked")?;
            if let Err(e) = injected {
                tracing::error!(error = %e, "注入失败");
                send(sink, Frame::status(seq, "error", &text, &e.to_string())).await?;
                return Ok(());
            }

            (state.on_text)(text.clone());
            send(sink, Frame::status(seq, "done", &text, "")).await?;
            send(sink, Frame::text(seq, &text)).await?;
        }
        PipelineEvent::Empty => {
            send(sink, Frame::status(seq, "done", "", "empty utterance")).await?;
        }
        PipelineEvent::Cancelled => {
            tracing::info!("PTT 已取消");
            send(sink, Frame::status(seq, "cancelled", "", "")).await?;
        }
        PipelineEvent::Ping => {
            send(sink, Frame::empty(FrameKind::Pong, seq)).await?;
        }
        PipelineEvent::Ignored => {}
    }
    Ok(())
}

async fn send(sink: &mut WsSink, frame: Frame) -> anyhow::Result<()> {
    sink.send(Message::Binary(frame.encode()))
        .await
        .context("websocket write failed")
}
