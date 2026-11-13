# AirMic Node 接线说明

目标主控：**ESP32-S3 SuperMini**（ESP32-S3FH4R2，4MB Flash + 2MB PSRAM，原生 USB-C，板载 WS2812 在 GPIO48）。PlatformIO 环境见 `platformio.ini` 的 `[env:supermini]`。

其他板（Lolin S3 Mini / XIAO ESP32S3 / DevKitC-1）把 `platformio.ini` 的 `default_envs` 改成 `esp32s3`，并核对本页末尾的「避开这些脚」。

> ⚠️ **别和 ESP32-C3 SuperMini 搞混**：两者外形几乎一样，但 C3 版是 RISC-V **单核 160MHz**、400KB SRAM、**无 PSRAM**、只引出 13 个 GPIO（0–10、20、21，**GPIO12–17 被 Flash 占用**），板载灯是 **GPIO8 的蓝色单色 LED（低电平点亮）**，**没有 GPIO48**。本页所有配置（`qio_qspi` PSRAM、GPIO48 灯）都只适用于 **S3** 版。

## ESP32-S3 SuperMini 兼容性

**固件引脚与 SuperMini 完全兼容，无需改任何 GPIO**：

| 功能 | GPIO | SuperMini 状况 |
|---|---|---|
| INMP441 `SCK` / BCLK | 4 | 安全 GPIO，已引出 |
| INMP441 `WS` / LRCK | 5 | 安全 GPIO，已引出 |
| INMP441 `SD` / DOUT | 6 | 安全 GPIO，已引出 |
| PTT 按键 | 7 | 安全 GPIO，已引出 |
| 单色状态灯（不用） | 2 | 安全 GPIO，已引出 |
| **板载 WS2812** | **48** | **板载 RGB，正好对应** |

SuperMini 已板载 **USB-C、LDO、BOOT 按键、WS2812**，所以外部只需接 **INMP441 + PTT 按键**。

```
ESP32-S3 SuperMini
  ├─ 3V3  ─── INMP441 VDD   (+ 100nF 到 GND)
  ├─ GND  ─── INMP441 GND
  ├─ GPIO4 ─── INMP441 SCK
  ├─ GPIO5 ─── INMP441 WS
  ├─ GPIO6 ─── INMP441 SD
  ├─ GND  ─── INMP441 L/R      ← 左声道
  └─ GPIO7 ─── PTT 按键 ── GND  ← 内部上拉，不用外部元件
  （WS2812 板载，无需接线；USB-C 供电 + 下载）
```

### 三个必须注意的点

1. **引脚是 2.54mm 间距，半孔 + 通孔混合焊盘**：可以焊排针插面包板做原型，也可以贴焊在载板上。不需要强制画 PCB。
2. **PSRAM 要在 `platformio.ini` 里显式声明**（已配好 `qio_qspi`）。本板是 **ESP32-S3FH4R2 = quad PSRAM**，`qio` 正确；只有换成 octal PSRAM 版本时才需要把 `board_build.arduino.memory_type` 改成 `opi_opi`、`board_build.psram_type` 改成 `opi`。若烧录后启动崩溃/反复重启，先核对芯片型号。
3. **板载 WS2812 在 GPIO48**（与板载红色状态灯共用），这是 S3 SuperMini 的固定设计。若你的板子是 **ESP32-C3 SuperMini**（注意：C3 版完全不同，见下），板载灯是 GPIO8 的蓝色单色 LED，代码和引脚都要改。

## 首次上电 / 配网

上电后如果 NVS 里没有凭据，固件会进入**配网模式**：

- 状态灯**青色慢闪**（单色 LED 为 1.6s 慢亮闪）
- 开热点 `AirMic-XXXXXX`（XXXXXX = MAC 后 6 位），打开 <http://192.168.4.1/> 填 Wi-Fi
- 也可以直接在串口发一行 `SSID:密码`，或 JSON `{"ssid":"..","pass":".."}`
- 已有凭据时，**开机 250ms 内按住 PTT** 可强制重新配网
- 等待上限 5 分钟，超时退回旧凭据（没有旧凭据就离线待机）

详见 [../README.md](../README.md) 的「网络配置」一节。

## 引脚分配（`include/airmic_config.h`）

| 功能 | 默认 GPIO | 可改 |
|---|---|---|
| INMP441 `SCK` / BCLK | 4 | `PIN_I2S_BCLK` |
| INMP441 `WS` / LRCK | 5 | `PIN_I2S_WS` |
| INMP441 `SD` / DOUT | 6 | `PIN_I2S_DIN` |
| PTT 按键 | 7 | `PIN_PTT` |
| 单色状态灯（不用） | 2 | `PIN_LED` |
| WS2812 RGB（板载） | 48 | `PIN_WS2812` |

**避开这些脚**：GPIO0 / 3 / 45 / 46（strapping）、19 / 20（USB D-/D+，SuperMini 已接 USB-C）、26~32（Flash / PSRAM）、33~37（Octal PSRAM 板）。

## INMP441

| INMP441 | ESP32-S3 | 备注 |
|---|---|---|
| VDD | 3.3V | 供电范围 1.62~3.6V，**不要接 5V** |
| GND | GND | 共地 |
| SCK | GPIO4 | I2S 位时钟，由 ESP32 主设备输出 |
| WS | GPIO5 | 左右声道选择 / 帧时钟 |
| SD | GPIO6 | 麦克风数据输出 → ESP32 输入 |
| L/R | **GND** | 选择左声道（`AIRMIC_I2S_LEFT_CHANNEL=1`）|

INMP441 输出 24bit 二补码、MSB first，左对齐在一个 32bit slot 里，低 8bit 补零。固件按 `32bit slot / Philips I2S / ONLY_LEFT` 读取，再 `>>16` 取高 16bit 得到 S16LE PCM。

> 采集结果全 0 或全是噪声时，先确认 L/R 确实接了 GND；若接的是 VDD，把 `AIRMIC_I2S_LEFT_CHANNEL` 改成 `0` 切右声道。

建议在 VDD 与 GND 之间并一个 0.1µF 去耦电容，麦克风尽量远离 Wi-Fi 天线与电源开关噪声。

## PTT 按键

```
GPIO7 ────┬──── [ 按键 ] ──── GND
          └── INPUT_PULLUP（固件内部开启）
```

按下为低电平（`AIRMIC_PTT_ACTIVE_LOW=1`）。若用外部上拉/高电平有效，把宏改成 `0`。

**行为**：按下立刻开始录音（不丢说话开头）；松开时按住时长 < 250ms 判为「取消」，发 `CANCEL` 丢弃整段；否则发 `END` 触发识别。

## 状态灯

方案 A（默认）：单色 LED + 限流电阻

```
GPIO2 ──── [ 330Ω ] ──── LED 阳极
LED 阴极 ─────────────── GND
```

高电平点亮（`AIRMIC_LED_ACTIVE_HIGH=1`）；若 LED 接在 3.3V 与 GPIO 之间（灌电流），改成 `0`。

方案 B：在 `include/secrets.h`（或 `include/airmic_config.h`）里设 `#define AIRMIC_LED_WS2812 1`，用板载 RGB（默认 GPIO48），颜色区分状态，无需外接元件。

### 灯效对照

| 状态 | 单色 LED | WS2812 |
|---|---|---|
| 上电 | 200ms 快闪 | 蓝快闪 |
| 配网中（SoftAP 已开） | 1.6s 慢亮闪 | 青慢闪 |
| 连 Wi-Fi | 400ms 快闪 | 蓝快闪 |
| Wi-Fi 已连 / WS 未连 | 1s 慢闪 | 紫慢闪 |
| 待机（已连接） | 2s 心跳慢闪 | 绿心跳慢闪 |
| 录音中 | **常亮** | 红常亮 |
| 识别 / 上传中 | 200ms 快闪 | 黄快闪 |
| 短按取消 | 300ms 短闪 | 白短闪 |
| 错误 | 三短闪 + 长灭（1.2s 周期）| 红三短闪 |

所有灯效都基于 `millis()` 相位计算，不 `delay()`，不会拖慢音频采集与网络收发。

## 电源

- USB 供电调试即可；做成电池设备时注意 Wi-Fi 发射峰值电流（建议 ≥500mA 能力），并给 3.3V LDO/DCDC 留足余量。
- 待机时固件开启 Wi-Fi modem sleep；按下 PTT 会临时关闭 sleep，降低 20ms 音频帧的上行抖动，松键后恢复。
