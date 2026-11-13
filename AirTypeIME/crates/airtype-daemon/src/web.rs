//! 内置设置页面：手写最小 HTTP/1.1 服务。
//!
//! 只需要 4 个路由（`/`、`/api/state`、`/api/config`、`/api/raw`），
//! 所以刻意不引入 axum / tiny_http 之类的依赖：用 tokio 的 TcpListener
//! 自己解析请求行与头部，几十行就能覆盖。
//!
//! 安全边界：默认只监听 127.0.0.1（见 `[web]` 段）。这个页面能改写
//! daemon 配置，**不要**把 bind 改成 0.0.0.0 后暴露在不可信网络里。

use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

use anyhow::{Context, anyhow};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

use airtype_core::Config;

use crate::registry::Registry;

/// 请求头大小上限，防止畸形请求把内存吃光。
const MAX_HEADER_BYTES: usize = 16 * 1024;

#[derive(Debug)]
struct Request {
    method: String,
    path: String,
    body: Vec<u8>,
}

#[derive(Clone)]
struct WebCtx {
    config: Config,
    config_path: PathBuf,
    registry: Arc<Registry>,
    started: Instant,
}

/// 启动设置页面服务，直到出错或进程退出。
pub async fn run_web(
    config: Config,
    config_path: PathBuf,
    registry: Arc<Registry>,
) -> anyhow::Result<()> {
    let addr = format!("{}:{}", config.web.bind, config.web.port);
    let requested: SocketAddr = addr
        .parse()
        .with_context(|| format!("invalid web.bind / web.port: {addr}"))?;
    let listener = TcpListener::bind(requested)
        .await
        .with_context(|| format!("bind {requested} failed（端口被占用？）"))?;

    tracing::info!(%addr, "设置页面已启动");
    let ctx = Arc::new(WebCtx {
        config,
        config_path,
        registry,
        started: Instant::now(),
    });

    loop {
        let (stream, peer) = listener.accept().await.context("accept failed")?;
        let ctx = ctx.clone();
        tokio::spawn(async move {
            if let Err(e) = handle_connection(stream, ctx).await {
                tracing::debug!(%peer, error = %e, "设置页面请求处理失败");
            }
        });
    }
}

async fn handle_connection(mut stream: TcpStream, ctx: Arc<WebCtx>) -> anyhow::Result<()> {
    let req = match read_request(&mut stream).await? {
        Some(r) => r,
        None => return Ok(()),  // 对端直接断开
    };

    match (req.method.as_str(), req.path.as_str()) {
        ("GET", "/") | ("GET", "/index.html") => {
            let body = render_page(&ctx).into_bytes();
            respond(&mut stream, 200, "OK", "text/html; charset=utf-8", &body).await
        }
        ("GET", "/api/state") => {
            let state = serde_json::json!({
                "version": env!("CARGO_PKG_VERSION"),
                "uptime_ms": ctx.started.elapsed().as_millis() as u64,
                "config_path": ctx.config_path.to_string_lossy(),
                "config": ctx.config,
                "devices": ctx.registry.snapshot(),
            });
            let body = serde_json::to_vec(&state).context("serialize state")?;
            respond(&mut stream, 200, "OK", "application/json; charset=utf-8", &body).await
        }
        ("POST", "/api/config") => {
            let form = parse_form(&String::from_utf8_lossy(&req.body));
            let mut config = ctx.config.clone();
            let result = apply_form(&mut config, &form).and_then(|_| validate(&config));
            match result {
                Ok(()) => save_and_reply(&mut stream, &ctx, config).await,
                Err(e) => {
                    let page = render_message(&ctx, &format!("保存失败：{e}"), true);
                    respond(
                        &mut stream,
                        400,
                        "Bad Request",
                        "text/html; charset=utf-8",
                        &page.into_bytes(),
                    )
                    .await
                }
            }
        }
        ("POST", "/api/raw") => {
            let form = parse_form(&String::from_utf8_lossy(&req.body));
            let text = form.get("raw_toml").cloned().unwrap_or_default();
            match Config::from_toml(&text) {
                Ok(config) => save_and_reply(&mut stream, &ctx, config).await,
                Err(e) => {
                    let page = render_message(
                        &ctx,
                        &format!("TOML 解析失败：{e}\n（配置未改动）"),
                        true,
                    );
                    respond(
                        &mut stream,
                        400,
                        "Bad Request",
                        "text/html; charset=utf-8",
                        &page.into_bytes(),
                    )
                    .await
                }
            }
        }
        (method, path) if method == "GET" || method == "HEAD" => {
            tracing::debug!(%method, %path, "设置页面 404");
            respond(
                &mut stream,
                404,
                "Not Found",
                "text/plain; charset=utf-8",
                b"Not Found",
            )
            .await
        }
        _ => respond(
            &mut stream,
            405,
            "Method Not Allowed",
            "text/plain; charset=utf-8",
            b"Method Not Allowed",
        )
        .await,
    }
}

async fn save_and_reply(
    stream: &mut TcpStream,
    ctx: &WebCtx,
    config: Config,
) -> anyhow::Result<()> {
    let path = ctx.config_path.clone();
    // 写文件是阻塞操作，丢到 spawn_blocking，别卡住执行器
    let written = tokio::task::spawn_blocking(move || config.save(&path))
        .await
        .context("save worker panicked")?;
    match written {
        Ok(()) => {
            tracing::info!(path = %ctx.config_path.display(), "配置已由设置页面保存");
            let page = render_message(
                ctx,
                &format!(
                    "已保存到 {}。<br>重启 daemon 后生效（当前连接与识别不受影响）。",
                    ctx.config_path.display()
                ),
                false,
            );
            respond(
                stream,
                200,
                "OK",
                "text/html; charset=utf-8",
                &page.into_bytes(),
            )
            .await
        }
        Err(e) => {
            let page = render_message(ctx, &format!("写入失败：{e}"), true);
            respond(
                stream,
                500,
                "Internal Server Error",
                "text/html; charset=utf-8",
                &page.into_bytes(),
            )
            .await
        }
    }
}

async fn read_request(stream: &mut TcpStream) -> anyhow::Result<Option<Request>> {
    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 1024];

    let header_end = loop {
        if let Some(pos) = find(&buf, b"\r\n\r\n") {
            break pos + 4;
        }
        if buf.len() > MAX_HEADER_BYTES {
            return Err(anyhow!("请求头超过 {MAX_HEADER_BYTES} 字节"));
        }
        let n = stream.read(&mut tmp).await?;
        if n == 0 {
            return Ok(None);
        }
        buf.extend_from_slice(&tmp[..n]);
    };

    let head = String::from_utf8_lossy(&buf[..header_end]).to_string();
    let mut lines = head.lines();
    let request_line = lines.next().unwrap_or_default();
    let mut parts = request_line.split_whitespace();
    let method = parts.next().unwrap_or_default().to_string();
    let target = parts.next().unwrap_or("/").to_string();
    let path = target
        .split(['?', '#'])
        .next()
        .unwrap_or("/")
        .to_string();

    let mut content_length = 0usize;
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                content_length = v.trim().parse().unwrap_or(0);
            }
        }
    }

    let mut body = buf[header_end..].to_vec();
    while body.len() < content_length {
        let n = stream.read(&mut tmp).await?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }
    body.truncate(content_length);

    Ok(Some(Request {
        method,
        path,
        body,
    }))
}

async fn respond(
    stream: &mut TcpStream,
    status: u16,
    reason: &str,
    content_type: &str,
    body: &[u8],
) -> anyhow::Result<()> {
    let head = format!(
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\n\
         Content-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream.write_all(head.as_bytes()).await?;
    stream.write_all(body).await?;
    stream.flush().await?;
    Ok(())
}

fn find(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

/// 解析 `application/x-www-form-urlencoded`。
/// `+` 当空格、`%XX` 解码成字节；非法转义按字面量保留。
fn parse_form(body: &str) -> HashMap<String, String> {
    let mut out = HashMap::new();
    for pair in body.split('&') {
        if pair.is_empty() {
            continue;
        }
        let (k, v) = pair.split_once('=').unwrap_or((pair, ""));
        out.insert(percent_decode(k), percent_decode(v));
    }
    out
}

fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            b'%' if i + 2 < bytes.len() => {
                let hi = (bytes[i + 1] as char).to_digit(16);
                let lo = (bytes[i + 2] as char).to_digit(16);
                if let (Some(h), Some(l)) = (hi, lo) {
                    out.push((h * 16 + l) as u8);
                    i += 3;
                } else {
                    out.push(b'%');
                    i += 1;
                }
            }
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn apply_form(config: &mut Config, form: &HashMap<String, String>) -> Result<(), String> {
    if let Some(v) = form.get("server_bind") {
        config.server.bind = v.clone();
    }
    if let Some(v) = form.get("server_port") {
        config.server.port = parse_u16(v, "server.port")?;
    }
    if let Some(v) = form.get("server_path") {
        config.server.path = v.clone();
    }
    if let Some(v) = form.get("asr_engine") {
        config.asr.engine = v.clone();
    }
    if let Some(v) = form.get("asr_timeout_ms") {
        config.asr.timeout_ms = parse_u64(v, "asr.timeout_ms")?;
    }
    if let Some(v) = form.get("streaming_enabled") {
        config.streaming.enabled = v == "on" || v == "true" || v == "1";
    }
    if let Some(v) = form.get("streaming_chunk_ms") {
        config.streaming.chunk_ms = parse_u32(v, "streaming.chunk_ms")?;
    }
    if let Some(v) = form.get("streaming_replace") {
        config.streaming.replace = v == "on" || v == "true" || v == "1";
    }
    if let Some(v) = form.get("inject_mode") {
        config.inject.mode = v.clone();
    }
    if let Some(v) = form.get("inject_restore_clipboard") {
        config.inject.restore_clipboard = v == "on" || v == "true" || v == "1";
    }
    if let Some(v) = form.get("inject_paste_delay_ms") {
        config.inject.paste_delay_ms = parse_u64(v, "inject.paste_delay_ms")?;
    }
    for (field, key) in [
        ("trim_filler_words", "pp_trim_filler_words"),
        ("normalize_numbers", "pp_normalize_numbers"),
        ("ensure_punctuation", "pp_ensure_punctuation"),
        ("collapse_whitespace", "pp_collapse_whitespace"),
    ] {
        if let Some(v) = form.get(key) {
            let on = v == "on" || v == "true" || v == "1";
            match field {
                "trim_filler_words" => config.postprocess.trim_filler_words = on,
                "normalize_numbers" => config.postprocess.normalize_numbers = on,
                "ensure_punctuation" => config.postprocess.ensure_punctuation = on,
                "collapse_whitespace" => config.postprocess.collapse_whitespace = on,
                _ => {}
            }
        }
    }
    if let Some(v) = form.get("web_enabled") {
        config.web.enabled = v == "on" || v == "true" || v == "1";
    }
    if let Some(v) = form.get("web_bind") {
        config.web.bind = v.clone();
    }
    if let Some(v) = form.get("web_port") {
        config.web.port = parse_u16(v, "web.port")?;
    }
    Ok(())
}

fn validate(config: &Config) -> Result<(), String> {
    config
        .server
        .bind
        .parse::<IpAddr>()
        .map_err(|_| format!("server.bind {:?} 不是合法 IP 地址", config.server.bind))?;
    if config.server.port == 0 {
        return Err("server.port 不能为 0".into());
    }
    if config.streaming.chunk_ms == 0 {
        return Err("streaming.chunk_ms 不能为 0".into());
    }
    if config.web.enabled {
        config
            .web
            .bind
            .parse::<IpAddr>()
            .map_err(|_| format!("web.bind {:?} 不是合法 IP 地址", config.web.bind))?;
        if config.web.port == 0 {
            return Err("web.port 不能为 0".into());
        }
    }
    if config.inject.mode != "none" && config.inject.mode != "clipboard_paste" {
        return Err(format!(
            "inject.mode {:?} 只能是 none 或 clipboard_paste",
            config.inject.mode
        ));
    }
    Ok(())
}

fn parse_u16(v: &str, field: &str) -> Result<u16, String> {
    v.trim()
        .parse()
        .map_err(|_| format!("{field} = {v:?} 不是合法的 u16"))
}

fn parse_u32(v: &str, field: &str) -> Result<u32, String> {
    v.trim()
        .parse()
        .map_err(|_| format!("{field} = {v:?} 不是合法的 u32"))
}

fn parse_u64(v: &str, field: &str) -> Result<u64, String> {
    v.trim()
        .parse()
        .map_err(|_| format!("{field} = {v:?} 不是合法的 u64"))
}

fn html_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            _ => out.push(c),
        }
    }
    out
}

fn checked(v: bool) -> &'static str {
    if v {
        " checked"
    } else {
        ""
    }
}

/// 复选框关闭时浏览器不会提交该字段，服务端就分不出「关掉」和「没提交」。
/// 所以在每个复选框前放一个同名的 `value="off"` 隐藏输入：
/// 勾选时复选框排在后面，`parse_form` 后写的值覆盖掉隐藏值。
fn hidden_off(name: &str) -> String {
    format!("<input type=\"hidden\" name=\"{name}\" value=\"off\">")
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------

const STYLE: &str = r#"
*{box-sizing:border-box}
body{margin:0;padding:24px 16px;background:#f4f5f7;color:#1c1e21;
font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}
main{max-width:820px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px}
h2{font-size:16px;margin:26px 0 10px;color:#3c4043}
.meta{font-size:13px;color:#5f6368;margin:0 0 8px;line-height:1.6}
.tip{font-size:13px;color:#5f6368;line-height:1.6;margin:0 0 12px}
code{background:#f1f3f4;padding:1px 4px;border-radius:3px;
font-family:ui-monospace,Consolas,monospace;font-size:12px}
.card{background:#fff;border-radius:10px;padding:16px;margin-bottom:14px;
box-shadow:0 1px 3px rgba(0,0,0,.08)}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:8px 10px;font-size:13px;
border-bottom:1px solid #f1f3f4;vertical-align:top}
th{color:#5f6368;font-weight:600;white-space:nowrap}
tr:last-child td{border-bottom:0}
.muted{color:#80868b}
.on{color:#137333;font-weight:600}
.off{color:#c5221f;font-weight:600}
label{display:block;font-size:13px;margin:10px 0 4px;color:#3c4043}
input[type=text],input[type=number],select,textarea{
width:100%;padding:8px;font-size:14px;border:1px solid #dadce0;
border-radius:6px;font-family:inherit;background:#fff}
input:focus,select:focus,textarea:focus{outline:none;border-color:#1a73e8}
.row{display:flex;gap:12px;flex-wrap:wrap}
.row>div{flex:1;min-width:160px}
.check{display:flex;align-items:center;gap:6px;margin-top:12px;font-size:13px}
.check input{width:auto}
button{margin-top:16px;padding:10px 18px;font-size:14px;border:0;
border-radius:6px;background:#1a73e8;color:#fff;font-family:inherit;
cursor:pointer}
button:active{background:#1765cc}
details{margin-top:18px}
summary{cursor:pointer;font-size:14px;color:#1a73e8}
.msg{padding:12px;border-radius:8px;font-size:14px;line-height:1.6}
.ok{background:#e6f4ea;color:#137333}
.err{background:#fce8e6;color:#c5221f;white-space:pre-wrap}
fieldset{border:1px solid #e8eaed;border-radius:8px;margin:0 0 12px;
padding:4px 14px 12px}
legend{font-size:13px;color:#5f6368;padding:0 6px}
"#;

fn render_page(ctx: &WebCtx) -> String {
    let c = &ctx.config;
    let mut h = String::new();
    h.push_str("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">");
    h.push_str("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    h.push_str("<title>AirTypeIME 设置</title>");
    h.push_str("<style>");
    h.push_str(STYLE);
    h.push_str("</style></head><body><main>");
    h.push_str(&format!(
        "<h1>AirTypeIME</h1><p class=\"meta\">v{} · ws://{}:{}{} · ASR={} · inject={} · 运行 {} 秒</p>",
        env!("CARGO_PKG_VERSION"),
        html_escape(&c.server.bind),
        c.server.port,
        html_escape(&c.server.path),
        html_escape(&c.asr.engine),
        html_escape(&c.inject.mode),
        ctx.started.elapsed().as_secs()
    ));

    h.push_str("<h2>设备</h2><div class=\"card\">");
    h.push_str("<table><thead><tr><th>设备</th><th>地址</th><th>状态</th>\
               <th>识别</th><th>首字延迟</th><th>总耗时</th><th>最近文本</th></tr></thead>");
    h.push_str("<tbody id=\"device-rows\"><tr><td colspan=\"7\" class=\"muted\">加载中…</td></tr></tbody>");
    h.push_str("</table></div>");

    h.push_str("<h2>配置</h2>");
    h.push_str(&format!(
        "<p class=\"tip\">保存后写入 <code>{}</code>，<b>重启 daemon 后生效</b>；当前连接与正在进行的识别不受影响。</p>",
        html_escape(&ctx.config_path.display().to_string())
    ));

    h.push_str("<form method=\"post\" action=\"/api/config\">");

    h.push_str("<fieldset><legend>WebSocket 服务</legend><div class=\"row\">");
    h.push_str(&format!(
        "<div><label>bind</label><input type=\"text\" name=\"server_bind\" value=\"{}\"></div>",
        html_escape(&c.server.bind)
    ));
    h.push_str(&format!(
        "<div><label>port</label><input type=\"number\" name=\"server_port\" min=\"1\" max=\"65535\" value=\"{}\"></div>",
        c.server.port
    ));
    h.push_str(&format!(
        "<div><label>path</label><input type=\"text\" name=\"server_path\" value=\"{}\"></div>",
        html_escape(&c.server.path)
    ));
    h.push_str("</div></fieldset>");

    h.push_str("<fieldset><legend>ASR</legend><div class=\"row\">");
    h.push_str("<div><label>engine</label><select name=\"asr_engine\">");
    for (val, label) in [("mock", "mock"), ("command", "command"), ("sherpa", "sherpa")] {
        let sel = if c.asr.engine == val { " selected" } else { "" };
        h.push_str(&format!("<option value=\"{val}\"{sel}>{label}</option>"));
    }
    h.push_str("</select></div>");
    h.push_str(&format!(
        "<div><label>timeout_ms</label><input type=\"number\" name=\"asr_timeout_ms\" min=\"1\" value=\"{}\"></div>",
        c.asr.timeout_ms
    ));
    h.push_str("</div></fieldset>");

    h.push_str("<fieldset><legend>流式识别</legend>");
    h.push_str(&hidden_off("streaming_enabled"));
    h.push_str(&format!(
        "<label class=\"check\"><input type=\"checkbox\" name=\"streaming_enabled\"{}> enabled（边说边出字）</label>",
        checked(c.streaming.enabled)
    ));
    h.push_str(&format!(
        "<div class=\"row\"><div><label>chunk_ms</label><input type=\"number\" name=\"streaming_chunk_ms\" min=\"1\" value=\"{}\"></div>",
        c.streaming.chunk_ms
    ));
    h.push_str(&hidden_off("streaming_replace"));
    h.push_str(&format!(
        "<div><label class=\"check\"><input type=\"checkbox\" name=\"streaming_replace\"{}> replace（覆盖式实时上屏）</label></div></div>",
        checked(c.streaming.replace)
    ));
    h.push_str("</fieldset>");

    h.push_str("<fieldset><legend>文本注入</legend><div class=\"row\">");
    h.push_str("<div><label>mode</label><select name=\"inject_mode\">");
    for (val, label) in [("none", "none（只写日志）"), ("clipboard_paste", "clipboard_paste（Ctrl+V）")] {
        let sel = if c.inject.mode == val { " selected" } else { "" };
        h.push_str(&format!("<option value=\"{val}\"{sel}>{label}</option>"));
    }
    h.push_str("</select></div>");
    h.push_str(&format!(
        "<div><label>paste_delay_ms</label><input type=\"number\" name=\"inject_paste_delay_ms\" min=\"0\" value=\"{}\"></div>",
        c.inject.paste_delay_ms
    ));
    h.push_str("</div>");
    h.push_str(&hidden_off("inject_restore_clipboard"));
    h.push_str(&format!(
        "<label class=\"check\"><input type=\"checkbox\" name=\"inject_restore_clipboard\"{}> restore_clipboard（注入后恢复原剪贴板）</label>",
        checked(c.inject.restore_clipboard)
    ));
    h.push_str("</fieldset>");

    h.push_str("<fieldset><legend>后处理</legend>");
    for (key, label, on) in [
        ("pp_trim_filler_words", "trim_filler_words", c.postprocess.trim_filler_words),
        ("pp_normalize_numbers", "normalize_numbers", c.postprocess.normalize_numbers),
        ("pp_ensure_punctuation", "ensure_punctuation", c.postprocess.ensure_punctuation),
        ("pp_collapse_whitespace", "collapse_whitespace", c.postprocess.collapse_whitespace),
    ] {
        h.push_str(&hidden_off(key));
        h.push_str(&format!(
            "<label class=\"check\"><input type=\"checkbox\" name=\"{key}\"{attr}> {label}</label>",
            attr = checked(on)
        ));
    }
    h.push_str("</fieldset>");

    h.push_str("<fieldset><legend>设置页面</legend>");
    h.push_str(&hidden_off("web_enabled"));
    h.push_str(&format!(
        "<label class=\"check\"><input type=\"checkbox\" name=\"web_enabled\"{}> enabled</label>",
        checked(c.web.enabled)
    ));
    h.push_str("<div class=\"row\">");
    h.push_str(&format!(
        "<div><label>bind</label><input type=\"text\" name=\"web_bind\" value=\"{}\"></div>",
        html_escape(&c.web.bind)
    ));
    h.push_str(&format!(
        "<div><label>port</label><input type=\"number\" name=\"web_port\" min=\"1\" max=\"65535\" value=\"{}\"></div>",
        c.web.port
    ));
    h.push_str("</div>");
    h.push_str(
        "<p class=\"tip\">保持 bind 为 <code>127.0.0.1</code>；改成 <code>0.0.0.0</code> 会让同局域网的人都能改这份配置。</p>",
    );
    h.push_str("</fieldset>");

    h.push_str("<button type=\"submit\">保存配置</button></form>");

    h.push_str(
        "<details><summary>原始 TOML（高级）</summary>\
         <p class=\"tip\">整份替换。保存前会先解析校验，语法错误不会覆盖现有配置。</p>\
         <form method=\"post\" action=\"/api/raw\">\
         <textarea name=\"raw_toml\" rows=\"22\" cols=\"80\">",
    );
    h.push_str(&html_escape(
        &c.to_toml().unwrap_or_else(|e| format!("# 序列化失败: {e}")),
    ));
    h.push_str("</textarea><button type=\"submit\">保存原始 TOML</button></form></details>");

    h.push_str("<script>");
    h.push_str(include_str!("web_page.js"));
    h.push_str("</script>");

    h.push_str("</main></body></html>");
    h
}

fn render_message(ctx: &WebCtx, message: &str, is_error: bool) -> String {
    let cls = if is_error { "err" } else { "ok" };
    let back = format!(
        "<p><a href=\"/\">← 返回设置页</a> · <code>{}</code></p>",
        html_escape(&ctx.config_path.display().to_string())
    );
    format!(
        "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">\
         <title>AirTypeIME</title><style>{STYLE}</style></head><body><main>\
         <h1>AirTypeIME</h1><div class=\"msg {cls}\">{message}</div>{back}\
         </main></body></html>"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_ctx() -> WebCtx {
        WebCtx {
            config: Config::default(),
            config_path: PathBuf::from("config/airtype.toml"),
            registry: Arc::new(Registry::new()),
            started: Instant::now(),
        }
    }

    #[test]
    fn form_parsing_handles_encoding() {
        let form = parse_form("a=hello+world&b=%E4%BD%A0%E5%A5%BD&c=1&empty=");
        assert_eq!(form.get("a").map(String::as_str), Some("hello world"));
        assert_eq!(form.get("b").map(String::as_str), Some("你好"));
        assert_eq!(form.get("c").map(String::as_str), Some("1"));
        assert_eq!(form.get("empty").map(String::as_str), Some(""));
    }

    #[test]
    fn form_parsing_keeps_bad_escapes_literal() {
        let form = parse_form("a=100%25&b=100%&c=%zz");
        assert_eq!(form.get("a").map(String::as_str), Some("100%"));
        assert_eq!(form.get("b").map(String::as_str), Some("100%"));
        assert_eq!(form.get("c").map(String::as_str), Some("%zz"));
    }

    #[test]
    fn form_without_equals_gets_empty_value() {
        let form = parse_form("flag");
        assert_eq!(form.get("flag").map(String::as_str), Some(""));
    }

    #[test]
    fn header_finder_handles_boundaries() {
        assert_eq!(find(b"abc\r\n\r\n", b"\r\n\r\n"), Some(3));
        assert_eq!(find(b"abc", b"\r\n\r\n"), None);
        assert_eq!(find(b"", b"\r\n\r\n"), None);
        assert_eq!(find(b"abc", b""), None);
    }

    #[test]
    fn checkbox_off_is_applied() {
        let mut c = Config::default();
        c.streaming.enabled = true;
        // 页面为每个复选框都放了 value="off" 的 hidden 哨兵，
        // 所以「关闭」和「开启」都是显式提交的。
        let form = parse_form("streaming_enabled=off&streaming_chunk_ms=250");
        apply_form(&mut c, &form).unwrap();
        assert!(!c.streaming.enabled);
        assert_eq!(c.streaming.chunk_ms, 250);
    }

    #[test]
    fn hidden_sentinel_is_overridden_when_checked() {
        // 隐藏输入在前、复选框在后，同名时 parse_form 保留后写的那个。
        let form = parse_form("streaming_enabled=off&streaming_enabled=on");
        assert_eq!(
            form.get("streaming_enabled").map(String::as_str),
            Some("on")
        );
    }

    #[test]
    fn all_checkbox_sentinels_are_rendered() {
        // 每个复选框都必须有同名 hidden 哨兵，否则关闭状态存不进去。
        let ctx = test_ctx();
        let page = render_page(&ctx);
        for name in [
            "streaming_enabled",
            "streaming_replace",
            "inject_restore_clipboard",
            "pp_trim_filler_words",
            "pp_normalize_numbers",
            "pp_ensure_punctuation",
            "pp_collapse_whitespace",
            "web_enabled",
        ] {
            let sentinel = format!("name=\"{name}\" value=\"off\"");
            assert!(page.contains(&sentinel), "缺少 {name} 的 hidden 哨兵");
        }
    }

    #[test]
    fn checkbox_on_means_true() {
        let mut c = Config::default();
        let form = parse_form("streaming_enabled=on");
        apply_form(&mut c, &form).unwrap();
        assert!(c.streaming.enabled);
    }

    #[test]
    fn form_values_are_applied() {
        let mut c = Config::default();
        let form = parse_form(
            "server_bind=0.0.0.0&server_port=9000&asr_engine=sherpa&inject_mode=clipboard_paste",
        );
        apply_form(&mut c, &form).unwrap();
        assert_eq!(c.server.bind, "0.0.0.0");
        assert_eq!(c.server.port, 9000);
        assert_eq!(c.asr.engine, "sherpa");
        assert_eq!(c.inject.mode, "clipboard_paste");
    }

    #[test]
    fn invalid_port_is_rejected() {
        let mut c = Config::default();
        let form = parse_form("server_port=notanumber");
        let err = apply_form(&mut c, &form).unwrap_err();
        assert!(err.contains("server.port"), "{err}");
    }

    #[test]
    fn invalid_bind_is_rejected() {
        let mut c = Config::default();
        c.server.bind = "localhost".into();
        let err = validate(&c).unwrap_err();
        assert!(err.contains("server.bind"), "{err}");
    }

    #[test]
    fn zero_chunk_is_rejected() {
        let mut c = Config::default();
        c.streaming.chunk_ms = 0;
        let err = validate(&c).unwrap_err();
        assert!(err.contains("chunk_ms"), "{err}");
    }

    #[test]
    fn unknown_inject_mode_is_rejected() {
        let mut c = Config::default();
        c.inject.mode = "telepathy".into();
        let err = validate(&c).unwrap_err();
        assert!(err.contains("inject.mode"), "{err}");
    }

    #[test]
    fn web_disable_skips_bind_validation() {
        let mut c = Config::default();
        c.web.enabled = false;
        c.web.bind = "not-an-ip".into();
        assert!(validate(&c).is_ok(), "web.enabled=false 时不该校验 bind");
    }

    #[test]
    fn html_escape_covers_textarea_hazard() {
        let escaped = html_escape("</textarea><script>");
        assert!(escaped.contains("&lt;/textarea&gt;"));
        assert!(!escaped.contains('<'));
    }

    #[test]
    fn default_config_passes_validation() {
        assert!(validate(&Config::default()).is_ok());
    }
}
