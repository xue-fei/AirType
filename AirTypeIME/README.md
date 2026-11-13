# AirTypeIME

AirType 的 PC 端：接收 AirMic Node 经 Wi-Fi WebSocket 送来的语音，调用 ASR，把文字写进当前输入框。

定位是 **「Wi-Fi 语音输入外设 + 专用 PC 输入法」**，不是标准蓝牙麦克风。

---

## 1. 架构：共享核心 + 平台原生外壳

输入法必须被操作系统识别为「输入法」才能往别的进程里写文字，这一层每个平台都有强制的原生技术栈，绕不开。所以把**跨平台业务逻辑收敛到 Rust 一处**，把**平台特异性隔离在薄外壳里**。

```text
[AirMic Node / ESP32-S3 SuperMini]  ──Wi-Fi WebSocket / 裸 PCM 16k16bit──┐
                                                                   ▼
┌──────────────────────────── AirTypeIME ──────────────────────────────────┐
│  airtype-daemon          WebSocket 服务 / 会话编排 / 注入（平台 I/O）     │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ airtype-core  纯同步、零异步运行时、零平台 API                     │  │
│  │  protocol → session(PCM 缓冲) → asr → postprocess → 最终文本       │  │
│  └───────────────────────────┬────────────────────────────────────────┘  │
│                              │ C ABI (airtype-ffi / cdylib)              │
│  ┌───────────────────────────▼────────────────────────────────────────┐  │
│  │ 原生外壳：Windows TSF/C++ · macOS IMK/ObjC++ · Linux Fcitx5/IBus   │  │
│  │ 职责：被系统识别为输入法、管理编辑会话、把回调文本写入输入框        │  │
│  └────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────┘
```

### 已确认的第一版决策

| 决策点 | 选定 | 理由 |
|---|---|---|
| 共享核心语言 | **Rust + C ABI** | 跨平台不返工；`airtype-core` 不依赖 tokio / Win32，可被任意外壳复用 |
| 首版交付 | **端到端可跑原型** | 先跑通「AirMic → PC → 输入框」，不啃 COM |
| 音频帧协议 | **裸 PCM 16kHz/16bit/mono S16LE** | 与既有三端音频流系统一致，256kbps 局域网扛得住；codec 可插拔 |
| Windows 注入 | **剪贴板 + 模拟 Ctrl+V**（默认 `none`） | 原型最快；正式输入法改用 TSF 编辑会话，不走剪贴板 |
| ASR | **本地离线 sherpa-onnx，Rust 内嵌**，可切换 `mock` / `command` / `sherpa` | 官方 Rust 绑定 crate，模型在 daemon 进程内加载，无 Python 依赖、无 HTTP 往返；默认走**流式 zipformer（fp32，中英）**，边说边出字 |

> **为什么第一版不做 TSF 外壳**：TSF 是注入到别的进程里的 in-proc COM DLL，要编译、注册、跨进程调试，还需要签名与管理权限。先验证链路价值更高。`platforms/windows/tsf/` 已备好 C ABI 头文件与接入说明，随时可接。

---

## 2. 目录结构

```
AirTypeIME/
├── Cargo.toml                       # workspace: core / daemon / ffi / asr-sherpa
├── rust-toolchain.toml              # stable (本机已装 1.99.1, msvc host)
├── .cargo/config.toml               # 指向 vendor/sherpa-libs（relative = true）
├── config/airtype.toml              # 默认配置（mock ASR）
├── config/airtype-command.toml      # command 引擎示例
├── config/airtype-sherpa.toml       # 内嵌 sherpa-onnx 配置（流式 / 整段，推荐）
├── docs/PROTOCOL.md                 # AirMic ↔ PC 帧协议 v1（8 字节头 + STATUS.timings）
├── models/                          # 本地 ONNX 模型（不入库，见 6.2）
├── vendor/sherpa-libs/              # sherpa-onnx 预编译 native 库归档（不入库，见 6.3）
├── crates/
│   ├── airtype-core/                # 平台无关核心（纯同步）
│   │   └── src/{protocol,audio,asr,streaming,postprocess,session,pipeline,config}.rs
│   ├── airtype-asr-sherpa/          # 内嵌 sherpa-onnx 引擎（静态链接 onnxruntime）
│   │   └── src/lib.rs
│   ├── airtype-daemon/              # WebSocket 服务 + 引擎装配 + 注入 + 延迟埋点
│   │   └── src/{lib,engines,server,inject,registry,web,main}.rs
│   │       # web_page.js 由 web.rs 用 include_str! 内嵌进页面
│   └── airtype-ffi/                 # cdylib，导出 C ABI
├── platforms/windows/tsf/
│   ├── include/airtype_ffi.h        # 原生外壳用的头文件
│   └── README.md                    # TSF 接入步骤
└── tools/
    ├── mock_airmic.py               # 零依赖模拟 AirMic 推流（后台读帧，可看实时 partial）
    ├── asr_bridge.py                # command 引擎的 ASR 桥接示例
    ├── fetch_sherpa_libs.sh         # 下载 sherpa-onnx 预编译 native 库（~118MB）
    └── bench_latency.py             # 端到端延迟基准（P50/P95 + 分阶段耗时 + 首字延迟）
```

---

## 3. 快速开始（Windows）

本机已装 `rustc 1.99.1 / x86_64-pc-windows-msvc`，MSVC 链接器验证可用。若换机器：

```bash
# 安装 Rust（https://rustup.rs），Windows 需 VS 2022 的 "Desktop development with C++"
rustup toolchain install stable --profile minimal
```

构建与测试：

```bash
cd AirTypeIME
cargo build --workspace --release
cargo test  --workspace
```

启动服务（第一个参数可指定配置文件路径）：

```bash
cargo run -p airtype-daemon -- config/airtype.toml
```

另开一个终端，模拟 AirMic 按下 PTT 说话再松键：

```bash
python tools/mock_airmic.py --duration-ms 1000
```

预期输出：

```
START  device=airmic-mock-01  codec=pcm_s16le  16000Hz/16bit/mono
AUDIO  50 帧 / 16000 samples
END
[STATUS] {"state":"recognizing", ...}
[STATUS] {"state":"done", "text":"你好，这是一次 AirType 语音输入测试。", ...}
[TEXT]   你好，这是一次 AirType 语音输入测试。
```

daemon 侧同时会出现 `最终文本: ...` 与 `[INJECT:none] ...`。

### 设置页面

daemon 启动后会额外开一个本机 HTTP 服务（默认 `http://127.0.0.1:8766/`），可以：

- 看当前配置、运行时长、每个 AirMic 的连接状态与识别统计（3 秒轮询 `/api/state`）；
- 用表单改配置并写回 `airtype.toml`，或整份粘贴 TOML（保存前先解析校验，语法错误不覆盖现有配置）；
- 改动**重启 daemon 后生效**，当前连接与正在进行的识别不受影响。

四个接口：`GET /`、`GET /api/state`、`POST /api/config`、`POST /api/raw`。服务是手写的最小 HTTP/1.1（`web.rs`），没有引入 axum / tiny_http 之类依赖。

> **安全边界**：默认只监听 `127.0.0.1`。这个页面能改写 ASR 与注入配置，把 `bind` 改成 `0.0.0.0` 前请先确认网络可信。

### 让文字真的进输入框

默认 `inject.mode = "none"`（只写日志，不碰系统剪贴板）——这样无头验证不会误改你正在编辑的窗口。要真实注入，改配置：

```toml
[inject]
mode = "clipboard_paste"
restore_clipboard = true
```

它保存剪贴板 → 写入识别文本 → 模拟 `Ctrl+V` → 恢复原剪贴板。焦点在哪个输入框，文字就进哪个。

### 让 AirMic 真连上

```toml
[server]
bind = "0.0.0.0"   # 默认 127.0.0.1 只接受本机
```

确认防火墙放行 TCP 8765。

---

## 4. 配置说明（`config/airtype.toml`）

| 段 | 字段 | 默认 | 说明 |
|---|---|---|---|
| `server` | `bind` / `port` / `path` | `127.0.0.1` / `8765` / `/ws` | WebSocket 监听 |
| `audio` | `sample_rate` / `channels` / `bits_per_sample` | `16000` / `1` / `16` | 与 AirMic `START` 元数据对应 |
| `audio` | `max_utterance_ms` | `60000` | 单段上限，防内存膨胀 |
| `asr` | `engine` | `mock` | `mock` \| `command` \| `sherpa`（推荐，内嵌 sherpa-onnx） |
| `sherpa` | `model_dir` / `encoder` / `decoder` / `joiner` / `tokens` | 见 [`config/airtype-sherpa.toml`](config/airtype-sherpa.toml) | 模型目录与显式文件路径；留空自动扫描 fp32 版本 |
| `sherpa` | `num_threads` / `provider` / `modeling_unit` / `decoding_method` | `4` / `cpu` / `cjkchar` / `greedy_search` | sherpa-onnx 解码参数 |
| `sherpa` | `enable_endpoint` | `false` | AirType 用 PTT 松键收尾，不靠端点检测 |
| `asr` | `command` | `python tools/asr_bridge.py {wav}` | `{wav}` 换成临时 WAV 路径，**stdout 首行**为识别文本 |
| `asr` | `timeout_ms` | `20000` | 命令超时会被 kill |
| `streaming` | `enabled` / `chunk_ms` / `replace` | `false` / `200` / `true` | 内嵌 sherpa 下 `enabled=true` 即开流式实时上屏 |
| `postprocess` | 4 个开关 | 全 `true` | 口头词 / 全角数字 / 末尾标点 / 空白 |
| `inject` | `mode` | `none` | `none` \| `clipboard_paste` |
| `web` | `enabled` / `bind` / `port` | `true` / `127.0.0.1` / `8766` | 内置设置页面；`bind` 保持本机，改 `0.0.0.0` 会暴露给同局域网 |

日志级别用环境变量控制：`RUST_LOG=debug cargo run -p airtype-daemon`。

---

## 5. 帧协议摘要

所有业务帧是 WebSocket **binary**，8 字节小端头：`magic(0xA1) | version(0x01) | kind | flags | seq:u16 | payload_len:u16 | payload`。

| kind | 方向 | 名称 | payload |
|---|---|---|---|
| `0x01` | C→S | START | JSON 元数据（codec / 采样率 / device） |
| `0x02` | C→S | AUDIO | 裸 PCM S16LE，建议 20ms = 640 字节 |
| `0x03` | C→S | END | 空 |
| `0x04` | C→S | CANCEL | 空 |
| `0x05/06` | 双向 | PING/PONG | 空 |
| `0x81` | S→C | STATUS | JSON 状态/错误 |
| `0x82` | S→C | TEXT | UTF-8 最终文本 |

完整约束、时序与容错见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)。

---

## 6. 本地离线 ASR（k2-fsa/sherpa-onnx）

### 6.1 为什么是「Rust 内嵌」而不是独立服务

`command` 引擎每段语音都冷启动一个进程，ONNX 模型加载就要几秒，达不到「松键 0.3~1s 出字」的目标。第一版用独立 Python 常驻服务（`tools/asr_server.py`）+ `HttpAsr` 解决，代价是：多一个进程要单独启动运维、`airtype-core` 为一次本地调用手写最小 HTTP/1.1、每次识别多一次进程间往返与 JSON 序列化。现在改成 k2-fsa 官方 Rust 绑定 [`sherpa-onnx`](https://crates.io/crates/sherpa-onnx)，模型在 **daemon 进程内**加载：

```text
airtype-daemon (Rust)
   ├─ airtype-asr-sherpa ── OnlineRecognizer（sherpa-onnx，静态链接 onnxruntime）
   │     ├─ 整段路径：一次喂入 + input_finished
   │     └─ 流式路径：分块 accept_waveform + input_finished
   └─ airtype-core（Pipeline / postprocess / 注入）
```

- **零 Python 依赖**：不需要 `pip install sherpa-onnx`，不需要额外运行时；
- **模型只加载一次**：`OnlineRecognizer` 建一次，整段与流式共享同一实例（`OnlineRecognizer` 是 `Send + Sync`，可 `Arc` 跨线程共享）；
- **release 二进制约 21MB**：静态链接的 `onnxruntime.lib` 虽有 944MB，链接器 GC 会裁掉没用到的算子（实测 `target/release/airtype-daemon.exe` 21MB）；
- **链路开销压到个位数毫秒**：省掉 HTTP 往返与 JSON 序列化（对比见 6.5）。

`airtype-core` 保持纯同步、零平台 API，不认识 sherpa-onnx：`Pipeline::new` 接收外部装配好的 `Arc<dyn AsrEngine>` / `Option<Arc<dyn StreamingAsr>>`，由 `airtype-daemon` 的 `engines.rs` 按 `asr.engine` 分发。换引擎、加引擎都不动 core。

实现 trait 即可换引擎，`airtype-core` 上层一行不用改：

```rust
// 整段
pub trait AsrEngine: Send + Sync {
    fn name(&self) -> &str;
    fn transcribe(&self, samples: &[i16], sample_rate: u32) -> Result<String, AsrError>;
}

// 流式（并列的第二条路径，由 [streaming] enabled 切换）
pub trait StreamingAsr: Send + Sync {
    fn name(&self) -> &str;
    fn start(&self) -> Result<Box<dyn AsrStream>, AsrError>;
}
```

### 6.2 准备模型（不入库，**只要 fp32**）

```bash
mkdir -p models && cd models
curl -fLO https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2
# Windows 自带 tar（支持 bz2）；或用 7-Zip 解压
tar -xjf sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2
```

目录里要有 `encoder/decoder/joiner` 的 **fp32** 版本 + `tokens.txt`，`models/` 已被 `.gitignore` 忽略。

> **只加载 fp32**：`airtype-asr-sherpa` 的模型发现会跳过任何文件名含 `.int8` / `-int8` / `.fp16` / `.int4` / `.quant` / `.q4` 等量化标记的文件，找不到 fp32 直接报错并提示下载非量化包；显式在 `[sherpa]` 里指定量化文件也会被拒绝。量化版本识别质量明显更差，这是明确取舍。
>
> 同一个 fp32 流式 zipformer 模型**同时服务流式与整段两条路径**——不需要为「整段兜底」再准备第二个模型，也不受「SenseVoice 只有 int8」的限制。
>
> 实测：同目录下同时存在 `encoder-epoch-99-avg-1.int8.onnx` 与 `encoder-epoch-99-avg-1.onnx` 时，daemon 启动日志确认选中后者（fp32）。

### 6.3 准备 native 库（首次构建）

`sherpa-onnx-sys` 在编译期需要 sherpa-onnx 的预编译 native 库（win-x64 约 118MB）。默认从 GitHub releases 下载；网络受限时先跑 fetch 脚本：

```bash
tools/fetch_sherpa_libs.sh     # 下载到 vendor/sherpa-libs/（.gitignore 已忽略）
```

`.cargo/config.toml` 把 `SHERPA_ONNX_ARCHIVE_DIR` 指向该目录（`relative = true`，相对配置文件解析），之后正常 `cargo build` 即可，不用手动设环境变量。归档文件名必须与 crate 版本匹配（当前 `sherpa_onnx` 2.0.1 → `sherpa-onnx-v2.0.1-win-x64-static-MT-Release-lib.tar.bz2`）。

### 6.4 启动与验证

```bash
# 终端 1：daemon（模型在进程内加载，首次启动约 2.5s）
cargo run -p airtype-daemon -- config/airtype-sherpa.toml

# 终端 2：用真实中文 WAV 走一遍 PTT，看实时上屏
python tools/mock_airmic.py --wav "models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20/test_wavs/0.wav" --fast
```

流式模式下 `mock_airmic.py` 会在**推音途中**就打印 `[PARTIAL ...]`，最后给出 `[TEXT]` 与首字延迟；`STATUS(done)` 里带 `timings` 分阶段耗时（字段定义见 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)）。

把 `[streaming] enabled` 改成 `false` 就退回「松键后整段解码」，同一份模型、同一份配置，其余字段完全通用。

### 6.5 实测延迟（本机 CPU / 4 线程 / fp32 流式）

`tools/bench_latency.py` 复用同一 WebSocket 连接连续 PTT，测「发 END → 收 TEXT」的客户端体感延迟，并额外统计**首字延迟**（START → 第一个非空 partial）与 partial 次数。

真实节奏（`--realtime`，按 20ms 帧间隔发送，对应实际 PTT）：

| 场景 | 单段音频 | 首字（说多久才出第一个字） | 松键→文字 P95 | 目标 ≤1s |
|---|---|---|---|---|
| 中英混合语音（zipformer 自带 `0.wav`） | 10.05 s | 1251.1 ms | **42.8 ms** | ✅ |
| 中文语音（`2.wav`） | 4.69 s | 838.4 ms | **1.6 ms** | ✅ |

```bash
python tools/bench_latency.py --wav "<0.wav 路径>" --realtime --count 5
python tools/bench_latency.py --wav "<2.wav 路径>" --realtime --count 5
```

> **松键→文字极短（1.6~42.8ms）** 是因为文字在说话过程中已作为 partial 逐步上屏，松键时只剩最后一次「退格 + 追加」的增量修正。
>
> **首字 1.25s / 0.84s 是模型特性，不是链路问题**：流式 zipformer 需要约 1s 上下文才敢出第一个 token。
>
> **对比上一版独立 Python 服务**：同一 `0.wav`，松键→文字 P95 从 173.9ms 降到 **42.8ms**；`2.wav` 从 8.6ms 降到 **1.6ms**。省掉的正是 HTTP 往返与 JSON 序列化（旧版每档 42~72ms，含 ONNX 解码 ~45ms）。首字延迟与 partial 次数完全一致（16 / 12 次），说明模型行为没变，只是链路更快。

整段路径（`[streaming] enabled = false`，同一模型）实测：10.05s 音频 `asr_ms` **1050ms**，**RTF 0.104**。

> 结论：**流式路径的延迟瓶颈在模型上下文窗口，不在链路**。链路开销已压到个位数毫秒。下一步值得投入的是 TSF 外壳（免去剪贴板副作用）与上电实测真实链路。

### 6.6 其他引擎与扩展

- `config/airtype-command.toml` 保留 `command` 示例，适合接 whisper.cpp / 云端 SDK 做临时验证（`tools/asr_bridge.py`）。
- 要接云端 HTTP API 或新的本地引擎：在 `airtype-daemon/src/engines.rs` 的 `build()` 里加一个分支即可，`airtype-core` 一行不用改。
- 想接非 zipformer 模型（如 SenseVoice / paraformer）：给 `airtype-asr-sherpa` 加一个 `OnlineModelConfig` 变体或改用 `OfflineRecognizer`，fp32 过滤与文件解析已经就绪。

---

## 7. 跨平台扩展路线

| 平台 | 外壳技术栈 | 状态 |
|---|---|---|
| Windows | C++ / TSF (COM in-proc DLL) | 头文件 + 接入说明已备，未实现 |
| macOS | Objective-C++ / InputMethodKit | 复用同一 Rust 核心 + C ABI |
| Linux | C++ / Fcitx5 或 IBus addon | 复用同一 Rust 核心 + C ABI |

扩展时只写新外壳，**不动 `airtype-core`**。C ABI 契约在 `platforms/windows/tsf/include/airtype_ffi.h`：

```c
#include "airtype_ffi.h"

static void on_text(const char* utf8, void* ud) {
    /* marshal 到编辑会话线程，用 ITfInsertAtSelection 写入 */
}

airtype_init("config/airtype.toml");
airtype_set_text_callback(on_text, NULL);
airtype_start();
/* ... 退出时 ... */
airtype_stop();
```

FFI 启动时**强制把内置注入设为 `none`**，由外壳负责写入，避免重复输入。详见 [`platforms/windows/tsf/README.md`](platforms/windows/tsf/README.md)。

> Tauri / Electron 只适合做设置界面，不能作为输入法本体。内置的 `web.rs` 已经承担了设置页（改配置 + 看设备状态）；后续若要更重的 UI，可另做一个共享前端，走同一份 `airtype.toml` 与 `/api/*` 接口。

---

## 8. 当前限制 / 下一步

- 第一版按 **PTT 起止**切分语音段，未做 VAD；连接断开且未收到 `END` 会丢弃缓冲（避免半句话被打进输入框）。
- 未按 `seq` 做重传（WebSocket 跑在 TCP 上已保证有序），只记录乱序告警。
- `codec` 只接受 `pcm_s16le`；Opus / ADPCM 需在 `audio.rs` 加解码层。
- `clipboard_paste` 目前仅 Windows；macOS / Linux 应由原生外壳的编辑会话写入。
- **流式实时上屏的代价**：`replace = true` 时靠「模拟退格 N 次 + Ctrl+V」覆盖已上屏文本，会反复占用剪贴板，且要求焦点窗口的光标位置不动；退格次数多时肉眼可见文字闪动。正规解法是 TSF composition（预编辑串），需要 C++ COM 外壳，`platforms/windows/tsf/` 已备好接入说明。
- 流式模型无标点、英文按大写字母分词输出（transducer 特性）；`postprocess` 只在收尾补句末标点，partial 阶段刻意不补，避免标点闪烁。
- ASR 已内嵌 sherpa-onnx（Rust，零 Python），但只支持 zipformer transducer 一种模型结构；换 SenseVoice / paraformer 需要给 `airtype-asr-sherpa` 加配置变体。
- 6.4 的延迟是**本地回环 + 已生成的 WAV**，不含 Wi-Fi 传输、INMP441 采集与按键物理延迟，这些必须上电实测。
- 设置页面与设备注册表已可用（见 §3「设置页面」），但只记录**本机**连接状态：不做多设备切换/选路，也不把状态回传给 AirMic（电池、LED 等仍只在固件本地）。

许可：MIT（沿用仓库根 `LICENSE`）。
