# AirMic Node 接线说明

主控：**ESP32-S3 Mini**（PlatformIO `esp32-s3-devkitc-1`；若用 Lolin S3 Mini / XIAO ESP32S3，改 `platformio.ini` 的 `board` 并核对引脚）。

## 引脚分配（`include/airmic_config.h`）

| 功能 | 默认 GPIO | 可改 |
|---|---|---|
| INMP441 `SCK` / BCLK | 4 | `PIN_I2S_BCLK` |
| INMP441 `WS` / LRCK | 5 | `PIN_I2S_WS` |
| INMP441 `SD` / DOUT | 6 | `PIN_I2S_DIN` |
| PTT 按键 | 7 | `PIN_PTT` |
| 单色状态灯 | 2 | `PIN_LED` |
| WS2812 RGB（可选） | 48 | `PIN_WS2812` |

**避开这些脚**：GPIO0 / 3 / 45 / 46（strapping）、19 / 20（USB D-/D+）、26~32（Flash / PSRAM）、33~37（Octal PSRAM 板）。

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
