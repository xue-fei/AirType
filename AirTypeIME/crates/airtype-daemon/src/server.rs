//! WebSocket 服务端：每个 AirMic 连接独占一条 Pipeline。

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use anyhow::{anyhow, Context};
use futures_util::{SinkExt, StreamExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::oneshot;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;

use airtype_core::asr::AsrEngine;
use airtype_core::streaming::StreamingAsr;
use airtype_core::{Config, Frame, FrameKind, Pipeline, PipelineEvent, Timings};

use crate::engines;
use crate::inject::{build_injector, TextInjector};
use crate::registry::Registry;
use crate::TextCallback;

type WsSink = futures_util::stream::SplitSink<WebSocketStream<TcpStream>, Message>;

/// 一次 PTT 的流式观测：首字延迟（START → 首个 `Partial`）。
#[derive(Debug, Default)]
struct StreamTrace {
    started_at: Option<Instant>,
    first_partial_ms: u64,
}

impl StreamTrace {
    fn on_started(&mut self) {
        self.started_at = Some(Instant::now());
        self.first_partial_ms = 0;
    }

    fn on_partial(&mut self) {
        if self.first_partial_ms == 0 {
            if let Some(t) = self.started_at {
                self.first_partial_ms = t.elapsed().as_millis() as u64;
            }
        }
    }
}

struct ServerState {
    config: Config,
    asr: Arc<dyn AsrEngine>,
    streaming: Option<Arc<dyn StreamingAsr>>,
    injector: Arc<dyn TextInjector>,
    on_text: TextCallback,
    /// 设备状态快照，给设置页面用。
    registry: Arc<Registry>,
}

/// 启动服务，直到 `shutdown` 被触发。非阻塞于调用者之外（本身是 async）。
pub async fn run_server(
    config: Config,
    registry: Arc<Registry>,
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

    let engines = engines::build(&config)?;
    let asr = engines.asr;
    let injector = build_injector(&config.inject).context("build text injector")?;

    tracing::info!(
        %addr,
        asr = asr.name(),
        streaming = engines
            .streaming
            .as_ref()
            .map(|s| s.name())
            .unwrap_or("off"),
        inject = injector.name(),
        path = %config.server.path,
        "AirTypeIME WebSocket 服务已启动"
    );

    let state = Arc::new(ServerState {
        config,
        asr,
        streaming: engines.streaming,
        injector,
        on_text,
        registry,
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

    let pipeline = Arc::new(Mutex::new(
        Pipeline::new(&state.config, state.asr.clone(), state.streaming.clone())
            .context("build pipeline")?,
    ));
    let mut trace = StreamTrace::default();
    tracing::info!(%peer, "AirMic 已连接");
    // 先登记连接（此时还不知道 device 名），START 帧到达时再补名字。
    let peer_key = peer.to_string();
    state.registry.on_connected(&peer_key);
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
                // END/CANCEL 到达时刻是「松键」的服务端近似，用来算端到端总耗时。
                let end_at = if matches!(frame.kind, FrameKind::End | FrameKind::Cancel) {
                    Some(Instant::now())
                } else {
                    None
                };
                let p = pipeline.clone();
                let outcome = tokio::task::spawn_blocking(move || {
                    let mut guard = p
                        .lock()
                        .map_err(|_| anyhow!("pipeline mutex poisoned"))?;
                    let event = guard.on_frame(&frame).map_err(anyhow::Error::from)?;
                    let timing = guard.last_timing();
                    Ok::<_, anyhow::Error>((event, timing))
                })
                .await
                .context("pipeline worker panicked")?;

                match outcome {
                    Ok((event, timing)) => {
                        dispatch(
                            event,
                            timing,
                            end_at,
                            &mut trace,
                            &state,
                            &peer_key,
                            &mut sink,
                            seq,
                        )
                        .await?
                    }
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

    let seq_warnings = pipeline
        .lock()
        .map(|p| p.seq_warnings())
        .unwrap_or(0);
    state.registry.on_disconnected(&peer_key);
    tracing::info!(%peer, seq_warnings, "AirMic 断开");
    Ok(())
}

async fn dispatch(
    event: PipelineEvent,
    timing: Option<Timings>,
    end_at: Option<Instant>,
    trace: &mut StreamTrace,
    state: &Arc<ServerState>,
    peer: &str,
    sink: &mut WsSink,
    seq: u16,
) -> anyhow::Result<()> {
    match event {
        PipelineEvent::Started { device, sample_rate } => {
            tracing::info!(%device, %sample_rate, "PTT 开始");
            state.registry.on_device(peer, &device);
            trace.on_started();
            send(sink, Frame::status(seq, "recognizing", "", "")).await?;
        }
        PipelineEvent::Appended { samples, total_ms } => {
            tracing::trace!(%samples, %total_ms, "追加 PCM");
        }
        PipelineEvent::Recognized(text) => {
            tracing::info!(target: "airtype", "最终文本: {text}");

            let inject_started = Instant::now();
            let injector = state.injector.clone();
            let inject_text = text.clone();
            let injected = tokio::task::spawn_blocking(move || injector.inject(&inject_text))
                .await
                .context("injector worker panicked")?;
            let inject_ms = inject_started.elapsed().as_millis() as u64;
            if let Err(e) = injected {
                tracing::error!(error = %e, "注入失败");
                send(sink, Frame::status(seq, "error", &text, &e.to_string())).await?;
                return Ok(());
            }

            (state.on_text)(text.clone());
            let mut metrics = timing.unwrap_or_default();
            metrics.inject_ms = inject_ms;
            metrics.total_ms = end_at
                .map(|t| t.elapsed().as_millis() as u64)
                .unwrap_or(0);
            state
                .registry
                .on_utterance(peer, &text, trace.first_partial_ms, metrics.total_ms);
            tracing::info!(
                asr_ms = metrics.asr_ms,
                post_ms = metrics.post_ms,
                inject_ms = metrics.inject_ms,
                total_ms = metrics.total_ms,
                audio_ms = metrics.audio_ms,
                "识别耗时"
            );
            send(
                sink,
                Frame::status_with_timings(seq, "done", &text, "", Some(metrics)),
            )
            .await?;
            send(sink, Frame::text(seq, &text)).await?;
        }
        PipelineEvent::Partial {
            text,
            delete_chars,
            append,
        } => {
            trace.on_partial();
            if delete_chars > 0 || !append.is_empty() {
                let injector = state.injector.clone();
                let inject_append = append.clone();
                let replaced = tokio::task::spawn_blocking(move || {
                    injector.inject_replace(delete_chars, &inject_append)
                })
                .await
                .context("injector worker panicked")?;
                if let Err(e) = replaced {
                    // 注入失败不中断识别：后续 partial 会基于「已上屏文本」
                    // 继续算差异，但此时输入框已经和 pipeline 的 displayed
                    // 不一致了，只能靠下一次整体重写兜底。
                    tracing::error!(error = %e, delete_chars, "覆盖式注入失败");
                }
            }
            send(sink, Frame::status(seq, "partial", &text, "")).await?;
        }
        PipelineEvent::Final {
            text,
            delete_chars,
            append,
        } => {
            tracing::info!(target: "airtype", "流式最终文本: {text}");

            let inject_started = Instant::now();
            let injector = state.injector.clone();
            let inject_append = append.clone();
            let replaced = tokio::task::spawn_blocking(move || {
                injector.inject_replace(delete_chars, &inject_append)
            })
            .await
            .context("injector worker panicked")?;
            let inject_ms = inject_started.elapsed().as_millis() as u64;
            if let Err(e) = replaced {
                tracing::error!(error = %e, "最终注入失败");
                send(
                    sink,
                    Frame::status(seq, "error", &text, &e.to_string()),
                )
                .await?;
                return Ok(());
            }

            (state.on_text)(text.clone());
            let mut metrics = timing.unwrap_or_default();
            metrics.inject_ms = inject_ms;
            metrics.first_partial_ms = trace.first_partial_ms;
            metrics.total_ms = end_at
                .map(|t| t.elapsed().as_millis() as u64)
                .unwrap_or(0);
            state
                .registry
                .on_utterance(peer, &text, trace.first_partial_ms, metrics.total_ms);
            tracing::info!(
                asr_ms = metrics.asr_ms,
                post_ms = metrics.post_ms,
                inject_ms = metrics.inject_ms,
                first_partial_ms = metrics.first_partial_ms,
                total_ms = metrics.total_ms,
                audio_ms = metrics.audio_ms,
                "流式识别耗时"
            );
            send(
                sink,
                Frame::status_with_timings(seq, "done", &text, "", Some(metrics)),
            )
            .await?;
            send(sink, Frame::text(seq, &text)).await?;
        }
        PipelineEvent::CancelledStreaming { delete_chars } => {
            tracing::info!(delete_chars, "流式 PTT 已取消，撤销已上屏文本");
            if delete_chars > 0 {
                let injector = state.injector.clone();
                let undone = tokio::task::spawn_blocking(move || injector.inject_replace(delete_chars, ""))
                    .await
                    .context("injector worker panicked")?;
                if let Err(e) = undone {
                    tracing::error!(error = %e, delete_chars, "撤销注入失败");
                }
            }
            send(sink, Frame::status(seq, "cancelled", "", "")).await?;
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
