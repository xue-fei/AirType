#pragma once
// Wi-Fi STA 连接与断线重连；录音时临时关闭 modem sleep 降低上行延迟。

#include <Arduino.h>
#include <WiFi.h>

#include "airmic_config.h"

class WifiManager {
public:
    void begin();
    bool connected() const { return WiFi.status() == WL_CONNECTED; }

    /// 每轮调用；返回 true 表示「本次刚刚（重）连上」，调用方应重建 WS 连接。
    bool poll();
    void setHighPerformance(bool on);

private:
    uint32_t _lastAttempt   = 0;
    bool     _wasConnected  = false;
    bool     _highPerf      = false;
};
