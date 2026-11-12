#pragma once
// PTT 按键：消抖后输出 Press / Release 边沿事件。
// 按下即开始录音，松开时由主状态机根据按住时长决定 END 还是 CANCEL，
// 这样不会因为等待「短按判定」而丢掉说话开头。

#include <Arduino.h>

#include "airmic_config.h"

enum class PttEvent { None, Press, Release };

class PttButton {
public:
    void begin();
    PttEvent update();
    bool down() const { return _stableDown; }

private:
    bool readDown() const;

    bool     _rawDown    = false;
    bool     _stableDown = false;
    uint32_t _lastChange = 0;
};
