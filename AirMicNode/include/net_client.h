#pragma once
// WebSocketsClient 封装：只在这里调用 WS API，保证所有收发都发生在 loop 任务。

#include <Arduino.h>
#include <WebSocketsClient.h>

#include "airmic_config.h"
#include "protocol.h"
#include "wifi_credentials.h"

using ServerFrameCallback = void (*)(const airmic::ParsedFrame& frame, void* ctx);

class NetClient {
public:
    /// 用凭据里配过的地址（缺省退回编译期 AIRTYPE_HOST/PORT）建立连接。
    void begin(const airmic::WifiCredentials& creds);
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
    std::string           _host;
    uint16_t              _port = 0;
    volatile bool         _connected = false;
    ServerFrameCallback   _cb  = nullptr;
    void*                 _ctx = nullptr;
    static NetClient*     s_self;
};
