#include "wifi_manager.h"

void WifiManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);      // 不把凭据写进 NVS，方便换网络
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);         // 待机省电
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    _lastAttempt = millis();
    Serial.printf("[WiFi] 连接 SSID=%s ...\n", WIFI_SSID);
}

bool WifiManager::poll() {
    const bool up = connected();
    if (!up && (millis() - _lastAttempt) >= WIFI_RETRY_MS) {
        _lastAttempt = millis();
        Serial.println("[WiFi] 断开，主动重连");
        WiFi.reconnect();
    }
    const bool justConnected = up && !_wasConnected;
    if (justConnected) {
        Serial.printf("[WiFi] 已连接 IP=%s RSSI=%ddBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    _wasConnected = up;
    return justConnected;
}

void WifiManager::setHighPerformance(bool on) {
    if (on == _highPerf) {
        return;
    }
    _highPerf = on;
    // 录音时关闭 Wi-Fi modem sleep，避免 DTIM 周期把 20ms 音频帧攒批发出去。
    WiFi.setSleep(!on);
}
