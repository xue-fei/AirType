# AirMic Node ↔ AirTypeIME 帧协议 v1

> 第一版按 **裸 PCM / 16kHz / 16bit / 单声道 / S16LE** 定，编解码层留成可插拔，后续换 Opus 或 ADPCM 只改 `codec` 字段与 `audio.rs`，不动状态机与 ASR。

## 传输层

- WebSocket，端点 `ws://<host>:<port>/ws`，默认端口 `8765`。
- **业务帧一律为 binary 帧**。文本帧保留给未来的 JSON 调试/配置消息，v1 服务端收到后仅记录日志。
- WebSocket 本身跑在 TCP 上，已保证有序、不丢包；`seq` 只用于诊断与乱序告警，不做重传。

## 帧头（8 字节，全部 Little Endian）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | `magic` | 固定 `0xA1` |
| 1 | 1 | `version` | 固定 `0x01` |
| 2 | 1 | `kind` | 帧类型，见下表 |
| 3 | 1 | `flags` | 位标志，v1 固定 `0` |
| 4 | 2 | `seq` | u16，**连接内全局连续递增**，允许回绕；不按 PTT 段重置 |
| 6 | 2 | `payload_len` | u16，payload 字节数 |
| 8.. | n | `payload` | 依 `kind` 解释 |

单个 binary 帧总长 ≤ 8 + 4096 字节。

## 客户端 → 服务端（AirMic Node）

| kind | 名称 | payload | 说明 |
|---|---|---|---|
| `0x01` | `START` | UTF-8 JSON | 按键按下，开启一次语音段 |
| `0x02` | `AUDIO` | 裸 PCM S16LE | 长度必须为偶数，建议 20ms / 320 samples / 640 字节 |
| `0x03` | `END` | 空或 JSON | 松键，触发识别与注入 |
| `0x04` | `CANCEL` | 空 | 短按取消，丢弃已缓冲音频 |
| `0x05` | `PING` | 空 | 心跳，服务端回 `PONG` |
| `0x06` | `PONG` | 空 | 心跳响应 |

`START` payload 示例：

```json
{
  "device": "airmic-01",
  "codec": "pcm_s16le",
  "sample_rate": 16000,
  "channels": 1,
  "bits_per_sample": 16
}
```

`codec` 为 `pcm_s16le` 时 payload 直接是采样点；后续若引入 Opus/ADPCM，`codec` 改为对应标识，解码在 `audio.rs` 的 codec 层完成。

## 服务端 → 客户端

| kind | 名称 | payload | 说明 |
|---|---|---|---|
| `0x81` | `STATUS` | UTF-8 JSON | 状态与错误 |
| `0x82` | `TEXT` | UTF-8 | 经 ASR + 后处理、即将注入的最终文本 |
| `0x06` | `PONG` | 空 | 心跳响应 |

`STATUS` payload 示例（v1.1 起 `timings` 为可选字段）：

```json
{
  "state": "done",
  "text": "开饭时间早上9点至下午5点。",
  "message": "",
  "timings": {
    "asr_ms": 234,
    "post_ms": 0,
    "audio_ms": 5592,
    "inject_ms": 0,
    "total_ms": 234,
    "first_partial_ms": 185
  }
}
```

`state` 取值：`ready` / `recognizing` / `partial` / `done` / `cancelled` / `error`。

- `recognizing`：`START` 已收到，开始接收音频。
- `partial`：**仅流式模式**。每次增量识别的累积文本变化时发一帧，`text` 是当前累积全文（不是增量片段）。daemon 同时把「退格 + 追加」应用到输入框，实现实时上屏。
- `done`：本段结束，`text` 为最终文本，随后再发一帧 `TEXT`（空识别结果除外）。
- `cancelled`：`CANCEL` 已生效；流式模式下 daemon 会先撤销已上屏的字符。

`timings` 是**可选**的分阶段耗时（毫秒），只在识别成功的 `done` 里出现，用于定位松键延迟瓶颈：

| 字段 | 填写方 | 含义 |
|---|---|---|
| `asr_ms` | core | ASR 引擎耗时（进程内推理，无跨进程开销） |
| `post_ms` | core | 文本后处理耗时 |
| `audio_ms` | core | 本段语音时长，用于算实时率 RTF |
| `inject_ms` | daemon | 输入注入耗时 |
| `total_ms` | daemon | 收到 `END` → 注入完成的总耗时 |
| `first_partial_ms` | daemon | **仅流式**：`START` → 首个非空 `partial` 的耗时 |

**向后兼容**：旧客户端只取 `state` / `text`，忽略未知字段即可；AirMic 固件端解析 `STATUS` 也只读 `state`，新增 `partial` 与 `first_partial_ms` 无需改动固件。

## 一次 PTT 的完整时序

### 整段模式（`[streaming] enabled = false`，默认）

```
AirMic Node                         AirTypeIME
    |--- START(seq=0, meta) ----------->|  新建会话，清空 PCM 缓冲
    |--- AUDIO(seq=1, 640B) ---------->|  追加
    |--- AUDIO(seq=2, 640B) ---------->|  追加
    |            ...                   |
    |--- AUDIO(seq=N, 640B) ---------->|
    |--- END(seq=N+1) ---------------->|  PCM 拼接 -> ASR -> 后处理 -> 注入
    |<-- STATUS(done, text) -----------|
    |<-- TEXT(utf8) -------------------|
```

### 流式模式（`[streaming] enabled = true`，实时上屏）

```
AirMic Node                         AirTypeIME
    |--- START(seq=0, meta) ----------->|  开流（进程内建 OnlineStream）
    |<-- STATUS(recognizing) ----------|
    |--- AUDIO(seq=1, 640B) ---------->|  攒够 chunk_ms
    |--- AUDIO(seq=2, 640B) ---------->|  增量解码 -> 累积文本
    |<-- STATUS(partial, text) --------|  daemon: 退格 N + 追加 -> 输入框
    |            ...                   |
    |--- AUDIO(seq=N, 640B) ---------->|
    |<-- STATUS(partial, text) --------|
    |--- END(seq=N+1) ---------------->|  收尾解码（input_finished）
    |<-- STATUS(done, text, timings) --|  按 LCP 补最后一次「退格 + 追加」
    |<-- TEXT(utf8) -------------------|
```

`CANCEL` 在流式模式下直接丢弃进程内的 `OnlineStream`，并撤销本段已上屏的字符。

## ASR 引擎（进程内，独立于帧协议）

ASR 由 daemon 进程内的 `airtype-asr-sherpa`（sherpa-onnx 官方 Rust 绑定）承担，**没有跨进程调用**，帧协议中不出现 ASR 相关消息。

- 整段：`[streaming] enabled = false`，`END` 后整段喂入解码；
- 流式：`[streaming] enabled = true`，`START` 建 `OnlineStream`，`AUDIO` 按 `chunk_ms` 增量解码，`END` 收尾。

**只加载 fp32 模型**（文件名含 `.int8` / `-int8` / `.fp16` / `.int4` / `.quant` / `.q4` 等量化标记的一律跳过），详见 [`../crates/airtype-asr-sherpa/src/lib.rs`](../crates/airtype-asr-sherpa/src/lib.rs)。

## 约束与容错

- `START` 之前收到 `AUDIO`：回 `STATUS(error)` 并忽略该帧。
- `AUDIO` payload 为奇数长度、超过 4096 字节、或单段累计超过 `max_utterance_ms`：回 `STATUS(error)` 并终止该会话。
- 采样率/位深与 `START` 元数据不符：按元数据记录告警，v1 仍按配置的 16k/16bit 处理。
- `CANCEL` 随时可发，清空缓冲且不触发 ASR、不注入。
- 连接断开且未收到 `END`：丢弃缓冲，不注入（避免半句话被打进输入框）。
- 流式模式下 `partial` 注入失败（如焦点窗口切换导致退格落空）不中断识别：后续 `partial` 仍按「已上屏文本」算差异，最终由 `done` 的整体重写兜底。
