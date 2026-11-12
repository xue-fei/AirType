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
| 4 | 2 | `seq` | u16，单个 PTT 会话内从 0 递增，允许回绕 |
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

`STATUS` payload 示例：

```json
{"state":"done","text":"你好，这是一次测试。","message":""}
```

`state` 取值：`ready` / `recognizing` / `done` / `cancelled` / `error`。

## 一次 PTT 的完整时序

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

## 约束与容错

- `START` 之前收到 `AUDIO`：回 `STATUS(error)` 并忽略该帧。
- `AUDIO` payload 为奇数长度、超过 4096 字节、或单段累计超过 `max_utterance_ms`：回 `STATUS(error)` 并终止该会话。
- 采样率/位深与 `START` 元数据不符：按元数据记录告警，v1 仍按配置的 16k/16bit 处理。
- `CANCEL` 随时可发，清空缓冲且不触发 ASR、不注入。
- 连接断开且未收到 `END`：丢弃缓冲，不注入（避免半句话被打进输入框）。
