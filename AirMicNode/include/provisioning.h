#pragma once
// SoftAP 配网。
//
// 触发条件（见 main.cpp）：
//   - NVS 里没有可用凭据，且 secrets.h 也没填 SSID
//   - 开机 PTT_BOOT_HOLD_MS 内按住 PTT（强制重新配网）
//
// 进入后：
//   1. 开 AirMic-XXXXXX 开放热点（192.168.4.1）
//   2. WebServer 提供 GET /（表单 + 附近热点列表）、POST /save、GET /rescan
//   3. 同时监听串口：一行 "SSID:密码" 或 JSON {"ssid":"..","pass":".."}
//   4. 保存成功后返回 Saved，由调用方重启；超时返回 Timeout
//
// 安全说明：AP 不加密（本机没有屏幕，无法展示随机密码），配网热点只在
// 配网期间短时开启，保存后立即关闭。

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "airmic_config.h"
#include "status_led.h"
#include "wifi_credentials.h"

class Provisioning {
public:
    enum class Result { Saved, Timeout };

    /// 阻塞式配网：起热点与网页，等用户提交或超时。
    Result run(const String& apName, StatusLed& led);

    /// 喂一个串口字节；凑满一行且解析成功时返回 true 并填 out。
    bool serialFeed(uint8_t byte, airmic::WifiCredentials* out);

private:
    void startAp(const String& apName);
    void stopAp();
    void setupRoutes();
    void handleRoot();
    void handleSave();
    void handleRescan();
    void handleNotFound();
    String renderPage(const char* message, bool isError) const;
    void refreshScan();
    String htmlEscape(const String& in) const;

    WebServer    _server{80};
    String       _lineBuf;
    String       _scanSsids[PROVISION_SCAN_MAX];
    int          _scanRssi[PROVISION_SCAN_MAX];
    uint16_t     _scanCount = 0;
    bool         _saved     = false;
    uint32_t     _startMs   = 0;
};
