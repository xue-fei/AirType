#pragma once
// AirMic Node 集中配置：所有引脚、时序与网络参数都在这里改。
// Wi-Fi 与 PC 地址建议写在 include/secrets.h（已 gitignore），不要提交到仓库。

#include <string>

#include "wifi_credentials.h"

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

// ---- PC 端地址：NVS 优先，编译期默认兜底 --------------------------------
// 配网页面 / 串口 JSON 配过的 host/port 存在 NVS（airmic::WifiCredentials）；
// 没配过（hasHostPort() == false）就退回这里的编译期默认。
// 换 PC 时只需重新配网，不用改代码重刷固件。
inline std::string airmicEffectiveHost(const airmic::WifiCredentials& c) {
    return c.hasHostPort() ? c.host : std::string(AIRTYPE_HOST);
}
inline uint16_t airmicEffectivePort(const airmic::WifiCredentials& c) {
    return c.hasHostPort() ? c.port : static_cast<uint16_t>(AIRTYPE_PORT);
}

// ---- 配网（SoftAP）----------------------------------------------------
// 触发条件（见 main.cpp）：NVS 里没存过凭据且 secrets.h 也没填 SSID，
// 或者开机 PTT_BOOT_HOLD_MS 内按住 PTT（强制重新配网）。
// 配网热点 AirMic-XXXXXX 不加密，页面地址 http://192.168.4.1/
static constexpr uint32_t PROVISION_TIMEOUT_MS = 300000; // 配网等待上限（5 分钟）
static constexpr uint32_t PTT_BOOT_HOLD_MS     = 250;    // 开机按住 PTT 触发配网的窗口
static constexpr uint16_t PROVISION_SCAN_MAX   = 12;     // 页面最多列出多少个附近热点

// ---- 状态码/特性开关 --------------------------------------------------
// 1 = 使用板载 WS2812 RGB 灯（ESP32-S3 SuperMini 的板载灯在 GPIO48）；
// 0 = 外接单色 LED + 限流电阻（接 GPIO2）
#ifndef AIRMIC_LED_WS2812
#  define AIRMIC_LED_WS2812 1
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
// ESP32-S3 SuperMini 的 4/5/6/7/2 都在安全 GPIO 列表内，无需改动。
static constexpr int PIN_I2S_BCLK = 4;   // INMP441 SCK
static constexpr int PIN_I2S_WS   = 5;   // INMP441 WS / LRCK
static constexpr int PIN_I2S_DIN  = 6;   // INMP441 SD / DOUT
static constexpr int PIN_PTT      = 7;   // 按键 -> GND
static constexpr int PIN_LED      = 2;   // 单色状态灯（串 330Ω）

// 板载 WS2812 信号脚：ESP32-S3 SuperMini 固定在 GPIO48（与板载红色
// 状态灯共用）。可在 secrets.h 里覆盖。
// 注意：ESP32-C3 SuperMini 没有 GPIO48，板载的是 GPIO8 蓝色单色 LED，
// 那种板型要改 AIRMIC_LED_WS2812=0 并把 PIN_LED 改成 8。
#ifndef PIN_WS2812
#  define PIN_WS2812 48
#endif

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

// ---- 网页上传（没接麦克风 / PTT 时的替代输入，见 web_upload.cpp）--------------
// STA 连上后起 WebServer：录音页 + /upload（multipart 文件）+ /api/state。
// 收到的 WAV 剥掉头存进 PSRAM，再按协议帧重放给 daemon。
static constexpr uint16_t WEB_PORT            = 80;         // 网页服务端口
static constexpr size_t   WEB_PCM_MAX_BYTES   = 1500 * 1024;  // PCM 缓冲上限 ≈ 46.9s
static constexpr size_t   WEB_PSRAM_MARGIN    = 256 * 1024;   // 给协议栈/String 留的余量
static constexpr uint32_t WEB_REPLAY_GAP_MS   = 10;           // 重放每帧间隔（比实时快）
static constexpr uint32_t WEB_RECEIVE_TIMEOUT_MS = 30000;     // 半途断开的看门狗
static constexpr size_t   WEB_WAV_OVERHEAD_MAX = 2 * 1024;    // multipart 外层开销估算
