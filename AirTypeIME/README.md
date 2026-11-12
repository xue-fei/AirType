# AirTypeIME

AirType 的 PC 端：接收 AirMic Node 经 Wi-Fi WebSocket 送来的语音，调用 ASR，把文字写进当前输入框。

定位是 **「Wi-Fi 语音输入外设 + 专用 PC 输入法」**，不是标准蓝牙麦克风。

---

## 1. 架构：共享核心 + 平台原生外壳

输入法必须被操作系统识别为「输入法」才能往别的进程里写文字，这一层每个平台都有强制的原生技术栈，绕不开。所以把**跨平台业务逻辑收敛到 Rust 一处**，把**平台特异性隔离在薄外壳里**。

```text
[AirMic Node / ESP32-S3 Mini]  ──Wi-Fi WebSocket / 裸 PCM 16k16bit──┐
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
| ASR | **可切换适配层**（`mock` / `command`） | 离线可跑通；`command` 可接 whisper.cpp / faster-whisper / 云端 SDK |

> **为什么第一版不做 TSF 外壳**：TSF 是注入到别的进程里的 in-proc COM DLL，要编译、注册、跨进程调试，还需要签名与管理权限。先验证链路价值更高。`platforms/windows/tsf/` 已备好 C ABI 头文件与接入说明，随时可接。

---

## 2. 目录结构

```
AirTypeIME/
├── Cargo.toml                       # workspace: core / daemon / ffi
├── rust-toolchain.toml              # stable (本机已装 1.99.1, msvc host)
├── config/airtype.toml              # 默认配置
├── docs/PROTOCOL.md                 # AirMic ↔ PC 帧协议 v1（8 字节头）
├── crates/
│   ├── airtype-core/                # 平台无关核心（纯同步）
│   │   └── src/{protocol,audio,asr,postprocess,session,pipeline,config}.rs
│   ├── airtype-daemon/              # WebSocket 服务 + 注入
│   │   └── src/{lib,server,inject,main}.rs
│   └── airtype-ffi/                 # cdylib，导出 C ABI
├── platforms/windows/tsf/
│   ├── include/airtype_ffi.h        # 原生外壳用的头文件
│   └── README.md                    # TSF 接入步骤
└── tools/
    ├── mock_airmic.py               # 零依赖模拟 AirMic 推流
    └── asr_bridge.py                # command 引擎的 ASR 桥接示例
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
| `asr` | `engine` | `mock` | `mock` \| `command` |
| `asr` | `command` | `python tools/asr_bridge.py {wav}` | `{wav}` 换成临时 WAV 路径，**stdout 首行**为识别文本 |
| `asr` | `timeout_ms` | `20000` | 命令超时会被 kill |
| `postprocess` | 4 个开关 | 全 `true` | 口头词 / 全角数字 / 末尾标点 / 空白 |
| `inject` | `mode` | `none` | `none` \| `clipboard_paste` |

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

## 6. 接真实 ASR

实现 `AsrEngine` trait 即可，`airtype-core` 上层一行不用改：

```rust
pub trait AsrEngine: Send + Sync {
    fn name(&self) -> &str;
    fn transcribe(&self, samples: &[i16], sample_rate: u32) -> Result<String, AsrError>;
}
```

仓库已备好 `config/airtype-command.toml` 示例配置，直接试 command 引擎：

```bash
cargo run -p airtype-daemon -- config/airtype-command.toml
python tools/mock_airmic.py --fast
```

两条现成路径：

1. **`command`（零额外依赖）**：改 `asr_bridge.py` 里的 `recognize()`，接 faster-whisper / whisper.cpp / 云端 SDK。Rust 侧只负责写临时 WAV、限时、取 stdout。
2. **新增 HTTP 引擎**：在 `crates/airtype-core/src/asr.rs` 加一个实现（POST WAV 到云端 API），并在 `build_engine()` 里注册一个新的 `engine` 名字。

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

> Tauri / Electron 只适合做设置界面，不能作为输入法本体。设置 UI 后续可独立做一个共享的前端，通过本地 IPC 读写 `airtype.toml`。

---

## 8. 当前限制 / 下一步

- 第一版按 **PTT 起止**切分语音段，未做 VAD；连接断开且未收到 `END` 会丢弃缓冲（避免半句话被打进输入框）。
- 未按 `seq` 做重传（WebSocket 跑在 TCP 上已保证有序），只记录乱序告警。
- `codec` 只接受 `pcm_s16le`；Opus / ADPCM 需在 `audio.rs` 加解码层。
- `clipboard_paste` 目前仅 Windows；macOS / Linux 应由原生外壳的编辑会话写入。
- 云端 ASR 尚未内置，走 `command` 桥接或直接实现 trait。
- 设置界面、多设备管理、配网、电池/LED 状态回传都还没做。

许可：MIT（沿用仓库根 `LICENSE`）。
