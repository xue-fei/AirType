#include "net_client.h"

namespace {
// 全局组帧缓冲，避免在 loop 任务栈上放 4KB。单实例、只在 loop 里调用，安全。
uint8_t g_frameBuf[airmic::HEADER_LEN + airmic::MAX_PAYLOAD];
}  // namespace

NetClient* NetClient::s_self = nullptr;

void NetClient::begin(const airmic::WifiCredentials& creds) {
    s_self = this;
    _host  = airmicEffectiveHost(creds);
    _port  = airmicEffectivePort(creds);
    _ws.onEvent(eventEntry);
    _ws.setReconnectInterval(WS_RECONNECT_MS);
    _ws.begin(_host.c_str(), _port, AIRTYPE_PATH);
    Serial.printf("[WS] 目标 ws://%s:%u%s（%s，自动重连 %lums）\n",
                  _host.c_str(), _port, AIRTYPE_PATH,
                  creds.hasHostPort() ? "NVS 配置" : "编译期默认",
                  static_cast<unsigned long>(WS_RECONNECT_MS));
}

void NetClient::restart() {
    _connected = false;
    _ws.disconnect();
    // Wi-Fi 刚恢复时重建 TCP 状态，比等库自己的重连定时器更快。
    _ws.begin(_host.c_str(), _port, AIRTYPE_PATH);
}

bool NetClient::send(uint8_t kind, uint16_t seq,
                     const uint8_t* payload, size_t len) {
    if (!_connected) {
        return false;
    }
    const size_t n = airmic::encodeFrame(kind, seq, payload, len,
                                         g_frameBuf, sizeof(g_frameBuf));
    if (n == 0) {
        Serial.printf("[WS] 组帧失败 kind=0x%02X len=%u\n", kind,
                      static_cast<unsigned>(len));
        return false;
    }
    return _ws.sendBIN(g_frameBuf, n);
}

void NetClient::eventEntry(WStype_t type, uint8_t* payload, size_t length) {
    if (s_self != nullptr) {
        s_self->handleEvent(type, payload, length);
    }
}

void NetClient::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            _connected = false;
            Serial.println("[WS] 断开");
            break;

        case WStype_CONNECTED:
            _connected = true;
            Serial.printf("[WS] 已连接 %.*s\n",
                          static_cast<int>(length),
                          payload ? reinterpret_cast<const char*>(payload) : "");
            break;

        case WStype_BIN: {
            airmic::ParsedFrame frame{};
            if (!airmic::decodeFrame(payload, length, &frame)) {
                Serial.printf("[WS] 丢弃非法 binary 帧 (%u bytes)\n",
                              static_cast<unsigned>(length));
                break;
            }
            if (_cb != nullptr) {
                _cb(frame, _ctx);
            }
            break;
        }

        case WStype_TEXT:
            Serial.printf("[WS] 文本帧（v1 忽略）: %.*s\n",
                          static_cast<int>(length),
                          payload ? reinterpret_cast<const char*>(payload) : "");
            break;

        case WStype_ERROR:
            Serial.println("[WS] 协议错误");
            break;

        default:
            break;
    }
}
