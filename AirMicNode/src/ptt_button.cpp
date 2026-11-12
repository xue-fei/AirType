#include "ptt_button.h"

void PttButton::begin() {
    pinMode(PIN_PTT, AIRMIC_PTT_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
    _rawDown    = readDown();
    _stableDown = _rawDown;
    _lastChange = millis();
    Serial.printf("[PTT] GPIO%d / %s有效 / 消抖 %lums\n",
                  PIN_PTT, AIRMIC_PTT_ACTIVE_LOW ? "低电平" : "高电平",
                  static_cast<unsigned long>(PTT_DEBOUNCE_MS));
}

bool PttButton::readDown() const {
    const int level = digitalRead(PIN_PTT);
    return AIRMIC_PTT_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
}

PttEvent PttButton::update() {
    const bool raw = readDown();
    const uint32_t now = millis();

    if (raw != _rawDown) {
        _rawDown    = raw;
        _lastChange = now;
    }
    if (raw != _stableDown && (now - _lastChange) >= PTT_DEBOUNCE_MS) {
        _stableDown = raw;
        return raw ? PttEvent::Press : PttEvent::Release;
    }
    return PttEvent::None;
}
