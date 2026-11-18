// AirMic Node 主状态机
//
//   PTT 按下 -> START + 开始 I2S 采集
//   按住期间 -> 每 20ms 一帧 AUDIO（640B S16LE）
//   松开     -> 排空在途音频帧 -> END；按住 < 250ms 则发 CANCEL
//   服务端   -> STATUS(recognizing/done/cancelled/error) + TEXT
//
// seq 采用全局连续递增（不做「每段从 0 重置」）：服务端 Session 用
// last_seq+1 做诊断，连续递增可避免每段开头都产生一条 seq 告警；
// 协议 v1 并不要求 START 的 seq 必须是 0。

#include <Arduino.h>

#include <cstring>
#include <string>

#include "airmic_config.h"
#include "audio_capture.h"
#include "credential_store.h"
#include "net_client.h"
#include "protocol.h"
#include "provisioning.h"
#include "ptt_button.h"
#include "status_led.h"
#include "wifi_manager.h"

namespace {

enum class DeviceState { Idle, Recording, Uploading };

AudioCapture audio;
PttButton   button;
StatusLed   led;
WifiManager wifi;
NetClient   net;

DeviceState g_state            = DeviceState::Idle;
uint16_t    g_seq              = 0;
uint32_t    g_pressMs          = 0;
uint32_t    g_uploadMs         = 0;
uint32_t    g_lastPingMs       = 0;
uint32_t    g_lastStatsMs      = 0;
uint32_t    g_errorUntilMs     = 0;
uint32_t    g_cancelUntilMs    = 0;
uint32_t    g_bootUntilMs      = 0;
uint32_t    g_framesSent       = 0;
bool        g_waitingRelease   = false;  // 超时强制结束后，等真实松键
String      g_deviceId;

uint16_t nextSeq() {
    return g_seq++;  // uint16 自然回绕，协议允许
}

void markError() {
    g_errorUntilMs = millis() + ERROR_LED_MS;
}

/// MAC 后 6 位，用于生成设备 ID 与配网热点名。
String macSuffix() {
    WiFi.mode(WIFI_STA);             // 打开射频才能读到 MAC
    String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
    mac.replace(":", "");
    return mac.substring(mac.length() > 6 ? mac.length() - 6 : 0);
}

String makeDeviceId() {
    if (std::strlen(AIRMIC_DEVICE_ID) > 0) {
        return String(AIRMIC_DEVICE_ID);
    }
    String suffix = macSuffix();
    suffix.toLowerCase();
    return "airmic-" + suffix;
}

/// 上电后短时间内按住 PTT = 强制进入配网模式。
bool waitPttHold() {
    const uint32_t until = millis() + PTT_BOOT_HOLD_MS;
    while (millis() < until) {
        button.update();
        if (button.down()) {
            return true;
        }
    }
    return false;
}

/// 把采集队列里的 PCM 全部发出去。发 END 之前必须调用一次，
/// 否则尾音会落在 END 之后被服务端丢弃。
void drainAudio() {
    PcmChunk chunk;
    while (audio.pop(&chunk)) {
        if (!net.connected()) {
            audio.flush();
            return;
        }
        if (net.send(airmic::K_AUDIO, nextSeq(), chunk.pcm, chunk.len)) {
            ++g_framesSent;
        }
    }
}

void onServerFrame(const airmic::ParsedFrame& f, void*) {
    switch (f.kind) {
        case airmic::K_STATUS: {
            const airmic::ServerState st =
                airmic::parseStatusState(f.payload, f.payloadLen);
            Serial.printf("[STATUS] seq=%u state=%s payload=%.*s\n",
                          f.seq, airmic::serverStateName(st),
                          static_cast<int>(f.payloadLen),
                          reinterpret_cast<const char*>(f.payload));
            if (g_state == DeviceState::Uploading &&
                (st == airmic::ServerState::Done ||
                 st == airmic::ServerState::Cancelled ||
                 st == airmic::ServerState::Error)) {
                g_state = DeviceState::Idle;
            }
            if (st == airmic::ServerState::Error) {
                markError();
            }
            break;
        }
        case airmic::K_TEXT:
            Serial.printf("[TEXT]   %.*s\n",
                          static_cast<int>(f.payloadLen),
                          reinterpret_cast<const char*>(f.payload));
            break;
        case airmic::K_PONG:
            // 心跳正常，无需处理
            break;
        default:
            break;
    }
}

void finishRecording(bool cancel, bool forced) {
    audio.setRecording(false);
    wifi.setHighPerformance(false);
    drainAudio();  // 关键：先排空在途 AUDIO，再发收尾帧

    const uint8_t kind = cancel ? airmic::K_CANCEL : airmic::K_END;
    const bool sent = net.send(kind, nextSeq(), nullptr, 0);
    Serial.printf("[PTT] %s (%lu ms)\n",
                  cancel ? "CANCEL" : "END",
                  static_cast<unsigned long>(millis() - g_pressMs));

    if (cancel) {
        g_cancelUntilMs = millis() + 600;
    }
    if (!sent) {
        g_state = DeviceState::Idle;
        markError();
        return;
    }
    g_state         = DeviceState::Uploading;
    g_uploadMs      = millis();
    g_waitingRelease = forced;
}

void handleButton(uint32_t now) {
    const PttEvent ev = button.update();

    if (ev == PttEvent::Press) {
        if (g_waitingRelease) {
            return;  // 超时强制结束后的残留按住，忽略
        }
        if (!wifi.connected() || !net.connected()) {
            Serial.println("[PTT] 网络未就绪，忽略按键");
            markError();
            return;
        }
        if (g_state == DeviceState::Uploading) {
            Serial.println("[PTT] 上一段仍在识别，忽略按键");
            return;
        }

        g_pressMs = now;
        g_state   = DeviceState::Recording;
        audio.flush();
        audio.setRecording(true);
        wifi.setHighPerformance(true);

        const std::string meta = airmic::startMeta(
            g_deviceId.c_str(), SAMPLE_RATE, 1, 16);
        if (!net.send(airmic::K_START, nextSeq(),
                      reinterpret_cast<const uint8_t*>(meta.data()), meta.size())) {
            Serial.println("[PTT] START 发送失败，回退到待机");
            audio.setRecording(false);
            wifi.setHighPerformance(false);
            g_state = DeviceState::Idle;
            markError();
            return;
        }
        Serial.println("[PTT] START");
        return;
    }

    if (ev == PttEvent::Release) {
        if (g_waitingRelease) {
            g_waitingRelease = false;
            return;
        }
        if (g_state == DeviceState::Recording) {
            finishRecording((now - g_pressMs) < PTT_CANCEL_MS, false);
        }
    }
}

void checkTimeouts(uint32_t now) {
    if (g_state == DeviceState::Recording && !net.connected()) {
        Serial.println("[WS] 录音中连接中断，丢弃当前语音段");
        audio.setRecording(false);
        wifi.setHighPerformance(false);
        audio.flush();
        g_state           = DeviceState::Idle;
        g_waitingRelease  = true;
        markError();
        return;
    }

    if (g_state == DeviceState::Recording &&
        (now - g_pressMs) >= MAX_RECORD_MS) {
        Serial.println("[PTT] 达到最大录音时长，自动结束");
        finishRecording(false, true);
        return;
    }

    if (g_state == DeviceState::Uploading &&
        (now - g_uploadMs) >= UPLOAD_TIMEOUT_MS) {
        Serial.println("[WS] 等待识别结果超时，回到待机");
        g_state = DeviceState::Idle;
        markError();
    }
}

void heartbeat(uint32_t now) {
    if (net.connected() && (now - g_lastPingMs) >= HEARTBEAT_MS) {
        g_lastPingMs = now;
        drainAudio();
        net.send(airmic::K_PING, nextSeq(), nullptr, 0);
    }
}

void updateLed(uint32_t now) {
    if (now < g_bootUntilMs) {
        led.set(LedState::Boot);
    } else if (now < g_errorUntilMs) {
        led.set(LedState::Error);
    } else if (now < g_cancelUntilMs) {
        led.set(LedState::Cancelled);
    } else if (!wifi.connected()) {
        led.set(LedState::WifiConnecting);
    } else if (g_state == DeviceState::Recording) {
        led.set(LedState::Recording);
    } else if (g_state == DeviceState::Uploading) {
        led.set(LedState::Uploading);
    } else if (!net.connected()) {
        led.set(LedState::WifiReady);
    } else {
        led.set(LedState::Idle);
    }
    led.update();
}

void printStats(uint32_t now) {
    if ((now - g_lastStatsMs) < STATS_INTERVAL_MS) {
        return;
    }
    g_lastStatsMs = now;
    Serial.printf("[STAT] heap=%uKB / 已发音频帧=%lu / 队列丢帧=%lu / "
                  "RSSI=%ddBm / Wi-Fi=%s / WS=%s\n",
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024),
                  static_cast<unsigned long>(g_framesSent),
                  static_cast<unsigned long>(audio.dropped()),
                  WiFi.RSSI(),
                  wifi.connected() ? "up" : "down",
                  net.connected() ? "up" : "down");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== AirMic Node / AirTypeIME 帧协议 v1 ===");

    led.begin();
    led.set(LedState::Boot);
    button.begin();
    if (!audio.begin()) {
        Serial.println("[I2S] 初始化失败：请检查接线与引脚配置");
        markError();
    }

    // ---- Wi-Fi 凭据：NVS 优先，secrets.h 兜底，都没有则配网 ----
    airmic::WifiCredentials creds;
    bool haveCreds = CredentialStore::load(&creds);
    if (!haveCreds && std::strlen(WIFI_SSID) > 0 &&
        std::strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0) {
        creds.ssid = WIFI_SSID;
        creds.pass = WIFI_PASS;
        haveCreds = creds.valid();
        if (haveCreds) {
            Serial.println("[WiFi] NVS 无凭据，使用 secrets.h 里的配置");
        }
    }

    // 已有凭据时，开机按住 PTT 强制重新配网（换路由器 / 纠正错误凭据）
    bool forceProvisioning = !haveCreds;
    if (haveCreds && waitPttHold()) {
        Serial.println("[配网] 开机按住 PTT，强制进入配网模式");
        forceProvisioning = true;
    }

    if (forceProvisioning) {
        Provisioning prov;
        const String apName = "AirMic-" + macSuffix();
        if (prov.run(apName, led) == Provisioning::Result::Saved) {
            // run() 返回前已写入 NVS，重启后生效（ESP.restart 不会返回）
            ESP.restart();
            return;
        }
        // 超时：有旧凭据就继续用，没有就离线待机
        if (!CredentialStore::load(&creds)) {
            creds = airmic::WifiCredentials();
        }
    }

    if (creds.valid()) {
        wifi.begin(creds);  // 必须先初始化 Wi-Fi 才能读 MAC
    } else {
        WiFi.mode(WIFI_STA);  // 不连接，只为读 MAC
        Serial.println("[WiFi] 无可用凭据，离线待机（按住 PTT 开机可配网）");
    }
    g_deviceId = makeDeviceId();

    net.onServerFrame(onServerFrame, nullptr);
    net.begin(creds);  // 地址优先取 NVS 里配过的，缺省用编译期 AIRTYPE_HOST/PORT

    g_bootUntilMs = millis() + 1000;
    Serial.printf("设备 ID: %s\n", g_deviceId.c_str());
    Serial.printf("PC 端: %s:%u（%s）\n", airmicEffectiveHost(creds).c_str(),
                  static_cast<unsigned>(airmicEffectivePort(creds)),
                  creds.hasHostPort() ? "NVS 配置" : "编译期默认");
    Serial.printf("音频: %luHz / 16bit / mono / %ums 一帧 (%uB PCM)\n",
                  static_cast<unsigned long>(SAMPLE_RATE),
                  FRAME_MS,
                  static_cast<unsigned>(PCM_BYTES_PER_FRAME));
    Serial.printf("交互: 短按 <%lums 取消 / 最长 %lus 自动结束\n",
                  static_cast<unsigned long>(PTT_CANCEL_MS),
                  static_cast<unsigned long>(MAX_RECORD_MS / 1000));
    Serial.println("待机中（状态灯慢闪）...");
}

void loop() {
    const uint32_t now = millis();

    net.loop();  // WebSocketsClient 必须被高频调用
    if (wifi.poll()) {
        Serial.println("[WiFi] 重新联网，重建 WebSocket 连接");
        net.restart();
    }

    handleButton(now);
    drainAudio();
    checkTimeouts(now);
    heartbeat(now);
    updateLed(now);
    printStats(now);

    delay(1);  // 让出 CPU 给 FreeRTOS IDLE / Wi-Fi 任务
}
