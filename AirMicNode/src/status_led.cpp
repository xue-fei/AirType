#include "status_led.h"

void StatusLed::begin() {
#if AIRMIC_LED_WS2812
    _px = new Adafruit_NeoPixel(1, PIN_WS2812, NEO_GRB + NEO_KHZ800);
    _px->begin();
    _px->setBrightness(90);
    _px->clear();
    _px->show();
    Serial.printf("[LED] WS2812 / GPIO%d\n", PIN_WS2812);
#else
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, AIRMIC_LED_ACTIVE_HIGH ? LOW : HIGH);
    Serial.printf("[LED] 单色 GPIO%d / %s电平点亮\n",
                  PIN_LED, AIRMIC_LED_ACTIVE_HIGH ? "高" : "低");
#endif
    _since = millis();
    _dirty = true;
}

void StatusLed::set(LedState state) {
    if (state == _state && !_dirty) {
        return;
    }
    _state  = state;
    _since  = millis();
    _dirty  = true;
    _lastOn = false;
}

/// 给定状态与相位，返回当前应亮还是灭。所有灯效都在 millis() 上计算，不 delay。
bool StatusLed::litAt(LedState state, uint32_t phase) {
    switch (state) {
        case LedState::Boot:           return (phase % 200)  < 100;  // 快闪：上电
        case LedState::WifiConnecting: return (phase % 400)  < 80;   // 快闪：连 Wi-Fi
        case LedState::WifiReady:      return (phase % 1000) < 80;   // 慢闪：WS 未连
        case LedState::Idle:           return (phase % 2000) < 60;   // 心跳慢闪：待机
        case LedState::Recording:      return true;                   // 常亮
        case LedState::Uploading:      return (phase % 200)  < 100;  // 快闪：识别中
        case LedState::Cancelled:      return (phase % 300)  < 40;   // 短闪：已取消
        case LedState::Error: {
            // 三短闪 + 长灭，肉眼易与其他状态区分
            const uint32_t p = phase % 1200;
            return p < 80 || (p >= 160 && p < 240) || (p >= 320 && p < 400);
        }
    }
    return false;
}

void StatusLed::output(bool on, LedState state) {
#if AIRMIC_LED_WS2812
    uint32_t color = 0;
    switch (state) {
        case LedState::Boot:
        case LedState::WifiConnecting: color = _px->Color(0, 0, 60);   break;  // 蓝
        case LedState::WifiReady:      color = _px->Color(50, 0, 50);  break;  // 紫
        case LedState::Idle:           color = _px->Color(0, 45, 0);   break;  // 绿
        case LedState::Recording:      color = _px->Color(70, 0, 0);   break;  // 红
        case LedState::Uploading:      color = _px->Color(60, 40, 0);  break;  // 黄
        case LedState::Cancelled:      color = _px->Color(40, 40, 40); break;  // 白
        case LedState::Error:          color = _px->Color(70, 0, 0);   break;  // 红
    }
    _px->setPixelColor(0, on ? color : 0);
    _px->show();
#else
    const int level = AIRMIC_LED_ACTIVE_HIGH
                          ? (on ? HIGH : LOW)
                          : (on ? LOW : HIGH);
    digitalWrite(PIN_LED, level);
#endif
}

void StatusLed::update() {
    const bool on = litAt(_state, millis() - _since);
    if (_dirty || on != _lastOn) {
        output(on, _state);
        _lastOn = on;
        _dirty  = false;
    }
}
