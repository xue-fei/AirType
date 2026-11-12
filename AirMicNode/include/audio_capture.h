#pragma once
// INMP441 I2S 采集：16kHz / 32bit slot -> S16LE 20ms 帧。
// 采集跑在独立的 FreeRTOS 任务（core 0），WebSocket 只在 loop（core 1）里调用，
// 两者通过定长队列传递 PCM，避免跨任务调用非线程安全的 WebSocketsClient。

#include <Arduino.h>
#include <atomic>

#include "airmic_config.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

struct PcmChunk {
    uint16_t len;
    uint8_t  pcm[PCM_BYTES_PER_FRAME];
};

class AudioCapture {
public:
    bool begin();
    void setRecording(bool on);
    bool recording() const { return _recording.load(); }

    /// 从队列取一帧；返回 false 表示当前没有待发数据。
    bool pop(PcmChunk* out);
    void flush();
    uint32_t dropped() const { return _dropped.load(); }

private:
    static void taskEntry(void* arg);
    void run();

    QueueHandle_t         _queue = nullptr;
    TaskHandle_t          _task  = nullptr;
    std::atomic<bool>     _recording{false};
    std::atomic<uint32_t> _dropped{0};
    bool                  _driverReady = false;
};
