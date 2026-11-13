# AirMic Node

AirType 的硬件端：ESP32-S3 SuperMini + INMP441 数字麦克风 + PTT 按键 + 板载 WS2812，通过 Wi-Fi WebSocket 把语音推给 PC 端 [AirTypeIME](../AirTypeIME/README.md)。

**定位**：Wi-Fi 语音输入外设，不是蓝牙麦克风。不实现 ASR，ASR 与文本注入全在 PC 端。

## 已实现功能

| 功能 | 实现 |
|---|---|
| I2S 采集 | INMP441，16kHz / 32bit slot → S16LE，20ms / 320 samples / 640B 一帧 |
| 音频传输 | 裸 PCM，WebSocket binary 帧，帧协议 v1（8 字节头） |
| PTT | 消抖、按下即录、短按 `<250ms` 取消、松键触发识别 |
| 状态灯 | 单色 GPIO 或板载 WS2812，非阻塞灯效区分 9 种状态 |
| 配网 | SoftAP 热点 + 网页（`http://192.168.4.1/`）+ 串口回退，凭据存 NVS，换路由器不用重刷固件 |
| 网络 | Wi-Fi STA 断线重连、WebSocket 自动重连、Wi-Fi 恢复后主动重建连接 |
| 心跳 | 每 5s `PING`，服务端回 `PONG` |
| 服务端消息 | 解析 `STATUS` / `TEXT`，串口打印识别结果 |
| 保护 | 50s 自动结束、上传超时、录音中断网丢弃缓冲、队列满丢最旧帧 |

## 目录

```
AirMicNode/
├── platformio.ini              supermini（目标板）/ esp32s3（通用底板）/ native（协议单测）
├── include/
│   ├── airmic_config.h         引脚 / 时序 / 特性开关（集中配置）
│   ├── secrets.h.example       Wi-Fi 与 PC 地址模板（NVS 没配过时的兜底）
│   ├── protocol.h              帧协议 v1（纯标准 C++，无 Arduino 依赖）
│   ├── wifi_credentials.h      凭据解析 / 格式化 / 校验（纯标准 C++）
│   ├── credential_store.h      凭据的 NVS 读写（Preferences）
│   ├── provisioning.h          SoftAP 配网（网页 + 串口）
│   ├── audio_capture.h         I2S 采集 + FreeRTOS 队列
│   ├── ptt_button.h            按键消抖
│   ├── status_led.h            状态灯灯效
│   ├── wifi_manager.h          Wi-Fi 连接与省电
│   └── net_client.h            WebSocket 收发封装
├── src/                        对应实现 + main.cpp 主状态机
├── test/
│   ├── test_protocol/          native 单测（字节布局对齐 Rust 端）
│   └── test_wifi_credentials/  native 单测（凭据解析 / JSON / URL 解码）
├── tools/verify_ime_bridge.py  无硬件时打真实 PC 端的协议一致性验证
└── docs/WIRING.md              接线表与灯效表
```

## 快速开始

### 1. 网络配置

**方式 A：网页配网（推荐）**——不用填任何文件，直接烧录。上电后设备发现 NVS 里没有凭据，会自己开一个热点：

```
SSID:  AirMic-XXXXXX         （XXXXXX = 芯片 MAC 后 6 位）
地址:  http://192.168.4.1/    （热点不加密，状态灯青色慢闪）
```

用手机 / 电脑连上这个热点打开页面，选 Wi-Fi、填密码、保存，设备自动重启并接入。也可以不连热点，直接在串口发一行：

```
MyHomeWiFi:wifi-password
```

或等价的 JSON：`{"ssid":"MyHomeWiFi","pass":"wifi-password"}`（`password` 也行，密码留空表示开放网络）。

**方式 B：编译期写死（调试用）**

```bash
cp include/secrets.h.example include/secrets.h
```

编辑 `include/secrets.h`：填 `WIFI_SSID` / `WIFI_PASS`。**只在 NVS 里还没有已存凭据时生效**（配网成功后以 NVS 为准）。该文件已在 `.gitignore` 中。

**重新配网**：已有凭据时，**开机 250ms 内按住 PTT** 会强制进入配网模式，用来换路由器或纠正错误凭据。配网模式等 5 分钟超时，超时后退回旧凭据（没有旧凭据就离线待机）。

两种方式都不影响 `AIRTYPE_HOST`——PC 端地址仍然来自 `secrets.h`，要改运行 AirTypeIME 的 PC 的局域网 IP。PC 端需要监听局域网，把 `AirTypeIME/config/airtype.toml` 改成：

```toml
[server]
bind = "0.0.0.0"
port = 8765
```

### 2. 接线

按 [docs/WIRING.md](docs/WIRING.md) 接 INMP441、按键、状态灯。引脚不同就改 `include/airmic_config.h`。

### 3. 编译 / 烧录 / 监视

PlatformIO 用 `pip install --user platformio` 安装，`pio.exe` 不在 PATH 时统一用 `python -m platformio`：

```bash
cd AirMicNode
python -m platformio run -d . -e supermini            # 编译（目标板 ESP32-S3 SuperMini）
python -m platformio run -d . -e supermini -t upload  # 烧录（USB-C 口）
python -m platformio device monitor -d . -b 115200    # 串口日志（走 USB CDC）
python -m platformio test  -d . -e native             # 纯逻辑单测（帧协议 + 凭据解析），无需硬件
```

首次编译会自动下载 ESP32 platform、Xtensa 工具链与依赖库。

SuperMini 的引脚是 **2.54mm 间距、半孔 + 通孔混合焊盘**：焊排针可直插面包板做原型，也可以贴焊在载板上。板载 WS2812 在 **GPIO48**，灯效直接用，不用外接 LED。

### 4. 联调

```bash
# PC 端（另一个终端）
cd AirTypeIME && cargo run -p airtype-daemon -- config/airtype.toml

# 无硬件时，先验证「固件将发出的帧」能被 PC 端正确接受
cd AirMicNode && python tools/verify_ime_bridge.py --host 127.0.0.1
```

上电后串口应依次出现 `[WiFi] 已连接` → `[WS] 已连接` → 状态灯进入待机慢闪。按住 PTT 说话、松开，串口打印 `START / END / [STATUS] done / [TEXT] ...`。

## 帧协议 v1

完整规范见 [../AirTypeIME/docs/PROTOCOL.md](../AirTypeIME/docs/PROTOCOL.md)。固件侧只做这些事：

```
[0]=0xA1 [1]=0x01 [2]=kind [3]=0 [4..5]=seq:u16 LE [6..7]=payload_len:u16 LE [8..]=payload
```

| kind | 方向 | payload |
|---|---|---|
| `0x01` START | → | `{"device":"...","codec":"pcm_s16le","sample_rate":16000,"channels":1,"bits_per_sample":16}` |
| `0x02` AUDIO | → | 640B 裸 PCM S16LE |
| `0x03` END | → | 空 |
| `0x04` CANCEL | → | 空 |
| `0x05` PING / `0x06` PONG | ↔ | 空 |
| `0x81` STATUS | ← | JSON，`state` = ready / recognizing / done / cancelled / error |
| `0x82` TEXT | ← | UTF-8 最终文本 |

**seq 策略**：固件使用**全局连续递增**，不按 PTT 段重置。服务端 `Session` 用 `last_seq+1` 做乱序诊断，重置会导致每段开头多一条告警；协议 v1 未强制 `START` 的 seq 必须为 0，`u16` 自然回绕。

**时序保证**：松键时先 `drainAudio()` 把队列里在途的 AUDIO 全部发出，再发 END/CANCEL，避免尾音落在收尾帧之后被丢弃。连接断开且未收到 END 时，服务端按协议丢弃缓冲，不会把半句话打进输入框。

## 状态灯

见 [docs/WIRING.md](docs/WIRING.md) 的灯效表。**默认使用板载 WS2812**（SuperMini 的板载灯在 GPIO48），无需外接元件；设 `AIRMIC_LED_WS2812=0` 可改回 GPIO2 外接单色 LED（串 330Ω）。

## 关键配置（`include/airmic_config.h`）

| 项 | 默认 | 说明 |
|---|---|---|
| `AIRMIC_LED_WS2812` | 1 | 1 = 板载 WS2812 RGB（GPIO48）；0 = 外接单色 LED（GPIO2）|
| `PIN_WS2812` | 48 | 板载灯信号脚，可在 `secrets.h` 覆盖（个别板型为 GPIO21）|
| `AIRMIC_LED_ACTIVE_HIGH` | 1 | LED 电平极性 |
| `AIRMIC_PTT_ACTIVE_LOW` | 1 | 按键极性（内部上拉）|
| `AIRMIC_I2S_LEFT_CHANNEL` | 1 | 0 = 切右声道（L/R 接 VDD 时）|
| `AIRMIC_DEVICE_ID` | 空 | 空则由 MAC 生成 `airmic-XXXXXX` |
| `PTT_CANCEL_MS` | 250 | 短按取消阈值 |
| `MAX_RECORD_MS` | 50000 | 忘记松键的兜底（服务端 60000）|
| `AUDIO_QUEUE_LEN` | 8 | 采集→发送的帧队列深度 |
| `PROVISION_TIMEOUT_MS` | 300000 | 配网页面等待上限（5 分钟）|
| `PTT_BOOT_HOLD_MS` | 250 | 开机按住 PTT 强制配网的窗口 |
| `PROVISION_SCAN_MAX` | 12 | 配网页面最多列出多少个附近热点 |

## 验证记录

本机无硬件，验证覆盖「编译 + 协议字节 + 打真实 PC 端」三层：

| 项目 | 结果 |
|---|---|
| `pio run -e supermini` | **SUCCESS**；`firmware.bin` 897KB；RAM 15.4%（50484 / 327680 B）、Flash 70.1%（918617 / 1310720 B）；链接进 `sdk/esp32s3/qio_qspi/` 的 `spiram*.c.obj`，证明 quad PSRAM 已按 `qio` 编入 |
| `pio run -e esp32s3` | **SUCCESS**，无编译警告；`firmware.bin` 862KB；RAM 15.3%（50200 / 327680 B）、Flash 26.4%（882233 / 3342336 B） |
| `pio test -e native` | **22 / 22 PASSED**（`test_protocol` 9 + `test_wifi_credentials` 13）：帧头字节布局与 Rust `protocol.rs` 的 `header_layout_is_stable` 逐字节对齐；凭据解析覆盖 `"SSID:PASS"`、JSON 转义 / `\uXXXX` 代理对、URL 解码与 32/63 字节边界 |
| `tools/verify_ime_bridge.py` 打真实 daemon | **3 / 3 PASS**：正常 PTT（含 u16 seq 回绕 + PING/PONG）、连续 seq 多段会话、短按取消；服务端均正常回 `done` + `TEXT` / `cancelled` 且无 `error` |

工具链：PlatformIO Core 6.2.0 / espressif32 6.13.0 / Arduino-ESP32 2.0.17（IDF 4.4）/ WebSockets 2.7.3 / Adafruit NeoPixel 1.15.5。

`supermini` 与 `esp32s3` 两个 env 的差异只在构建配置（Flash 4MB / quad PSRAM / USB CDC），**GPIO 完全一致**，固件源码无差异。

**仍需上电实测**：INMP441 实际波形与左右声道选择、Wi-Fi 上行吞吐与丢帧、按键消抖手感、板载 WS2812 实灯、整段端到端延迟。这些无法在 PC 上验证。

## 已知限制 / 下一步

- **配网安全**：配网热点不加密（本机没有屏幕可展示随机密码），密码经明文 HTTP 提交，仅建议在家庭局域网内使用；配网完成即关闭热点。后续可加 DNS 强制门户（`/generate_204` 已做 302 跳转）与 HTTPS。
- **PC 端地址不可配**：`AIRTYPE_HOST` / `AIRTYPE_PORT` 仍是编译期常量，换 PC 仍要改 `secrets.h` 重刷。`WifiCredentials` 的 JSON 结构已预留扩展位，加字段不影响旧数据解析。
- **连续按键**：上一段识别结果尚未返回时（`Uploading`），新按键会被忽略，避免并发 ASR 与 seq 错乱。
- **编码**：v1 裸 PCM 256kbps，与既有三端音频流系统一致；换 Opus / ADPCM 只需改 `START` 的 `codec` 与 PC 端 `audio.rs` 的解码层。
- **电源管理**：只做了 modem sleep 与录音时高性能切换，未做轻休眠/按键唤醒（会断 Wi-Fi，需重连策略配合）。
- **PC 端注入**：目前 AirTypeIME 默认是 `inject.mode = "none"`（只写日志）；要真把文字打进输入框需改成 `clipboard_paste`，或后续补 TSF 外壳。
- 音频采集跑在 core 0，WebSocket 跑在 Arduino loop（core 1），避免跨任务调用非线程安全的 `WebSocketsClient`。
