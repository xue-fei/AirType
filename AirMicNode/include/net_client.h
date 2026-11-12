#pragma once
// WebSocketsClient 封装：只在这里调用 WS API，保证所有收发都发生在 loop 任务。

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "airmic_config.h"
#include "protocol.h"

using ServerFrameCallback = void (*)(const airmic::ParsedFrame& frame, void* ctx);

class NetClient {
public:
    void begin();
    void restart();
    void loop() { _ws.loop(); }
    bool connected() const { return _connected; }

    bool send(uint8_t kind, uint16_t seq, const uint8_t* payload, size_t len);
    void onServerFrame(ServerFrameCallback cb, void* ctx) {
        _cb  = cb;
        _ctx = ctx;
    }

private:
    static void eventEntry(WStype_t type, uint8_t* payload, size_t length);
    void handleEvent(WStype_t type, uint8_t* payload, size_t length);

    WebSocketsClient      _ws;
    volatile bool         _connected = false;
    ServerFrameCallback   _cb  = nullptr;
    void*                 _ctx = nullptr;
    static NetClient*     s_self;
};
