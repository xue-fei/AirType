#pragma once
// AirMic Node 集中配置：所有引脚、时序与网络参数都在这里改。
// Wi-Fi 与 PC 地址建议写在 include/secrets.h（已 gitignore），不要提交到仓库。

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

// ---- 网络凭据（由 secrets.h 覆盖） ------------------------------------
#ifndef WIFI_SSID
#  define WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef WIFI_PASS
#  define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif
#ifndef AIRTYPE_HOST
#  define AIRTYPE_HOST "192.168.1.100"
#endif
#ifndef AIRTYPE_PORT
#  define AIRTYPE_PORT 8765
#endif
#ifndef AIRTYPE_PATH
#  define AIRTYPE_PATH "/ws"
#endif

// ---- 状态码/特性开关 --------------------------------------------------
// 1 = 使用板载 WS2812 RGB 灯（GPIO 48 常见）；0 = 外接单色 LED + 限流电阻
#ifndef AIRMIC_LED_WS2812
#  define AIRMIC_LED_WS2812 0
#endif
// 1 = 高电平点亮（LED 串电阻接 GND）；板载共阳 LED 改成 0
#ifndef AIRMIC_LED_ACTIVE_HIGH
#  define AIRMIC_LED_ACTIVE_HIGH 1
#endif
// 1 = 按键另一端接 GND，内部上拉，按下为低电平
#ifndef AIRMIC_PTT_ACTIVE_LOW
#  define AIRMIC_PTT_ACTIVE_LOW 1
#endif
// INMP441 的 L/R 接 GND = 左声道，接 VDD = 右声道。
// 若采集出来全为 0 或噪声，把这里改成 0 即可切到右声道。
#ifndef AIRMIC_I2S_LEFT_CHANNEL
#  define AIRMIC_I2S_LEFT_CHANNEL 1
#endif
// 设备 ID；留空则用芯片 MAC 生成 airmic-XXXXXX
#ifndef AIRMIC_DEVICE_ID
#  define AIRMIC_DEVICE_ID ""
#endif

// ---- 引脚 --------------------------------------------------------------
// 避开 GPIO0/3/19/20/26..32/45/46（strapping、USB、Flash/PSRAM）。
static constexpr int PIN_I2S_BCLK = 4;   // INMP441 SCK
static constexpr int PIN_I2S_WS   = 5;   // INMP441 WS / LRCK
static constexpr int PIN_I2S_DIN  = 6;   // INMP441 SD / DOUT
static constexpr int PIN_PTT      = 7;   // 按键 -> GND
static constexpr int PIN_LED      = 2;   // 单色状态灯（串 330Ω）
static constexpr int PIN_WS2812  = 48;  // 板载 RGB（仅 AIRMIC_LED_WS2812=1 时有效）

// ---- 音频 --------------------------------------------------------------
// 协议 v1 固定：16kHz / 16bit / mono / S16LE，20ms 一帧。
static constexpr uint32_t SAMPLE_RATE        = 16000;
static constexpr uint16_t FRAME_MS           = 20;
static constexpr size_t   SAMPLES_PER_FRAME   = SAMPLE_RATE * FRAME_MS / 1000;  // 320
static constexpr size_t   PCM_BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2;          // 640
static constexpr uint8_t  I2S_RAW_BYTES_PER_SAMPLE = 4;  // INMP441: 24bit 左对齐于 32bit slot
static constexpr size_t   AUDIO_QUEUE_LEN    = 8;        // 640B × 8 ≈ 5KB

// ---- 交互与超时 --------------------------------------------------------
static constexpr uint32_t PTT_DEBOUNCE_MS   = 25;    // 按键消抖
static constexpr uint32_t PTT_CANCEL_MS     = 250;   // 短于此视为「取消」
static constexpr uint32_t MAX_RECORD_MS     = 50000; // 忘记松键的兜底（服务端上限 60000）
static constexpr uint32_t UPLOAD_TIMEOUT_MS = 12000; // 等待 done/cancelled/error
static constexpr uint32_t HEARTBEAT_MS      = 5000;  // PING 间隔
static constexpr uint32_t WIFI_RETRY_MS     = 3000;
static constexpr uint32_t WS_RECONNECT_MS   = 2000;
static constexpr uint32_t ERROR_LED_MS      = 2000;  // 错误灯效持续时间
static constexpr uint32_t STATS_INTERVAL_MS = 15000; // 串口打印堆/丢帧统计
