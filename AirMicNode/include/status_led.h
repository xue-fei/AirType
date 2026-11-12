#pragma once
// 非阻塞状态灯。默认外接单色 LED（闪烁模式区分状态），
// 把 AIRMIC_LED_WS2812 设为 1 可改用板载 RGB 灯（颜色区分状态）。

#include <Arduino.h>

#include "airmic_config.h"

#if AIRMIC_LED_WS2812
#  include <Adafruit_NeoPixel.h>
#endif

enum class LedState {
    Boot,
    WifiConnecting,
    WifiReady,
    Idle,
    Recording,
    Uploading,
    Cancelled,
    Error,
};

class StatusLed {
public:
    void begin();
    void set(LedState state);
    void update();

private:
    static bool litAt(LedState state, uint32_t phaseMs);
    void output(bool on, LedState state);

    LedState _state  = LedState::Boot;
    uint32_t _since  = 0;
    bool     _lastOn = false;
    bool     _dirty  = true;

#if AIRMIC_LED_WS2812
    Adafruit_NeoPixel* _px = nullptr;
#endif
};
