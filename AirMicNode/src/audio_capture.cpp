#include "audio_capture.h"

#include <cstring>

namespace {
constexpr size_t RAW_BYTES_PER_FRAME = SAMPLES_PER_FRAME * I2S_RAW_BYTES_PER_SAMPLE;
static_assert(RAW_BYTES_PER_FRAME == 1280, "20ms @16kHz 32bit slot must be 1280 bytes");
}  // namespace

bool AudioCapture::begin() {
    _queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(PcmChunk));
    if (_queue == nullptr) {
        Serial.println("[I2S] 队列创建失败");
        return false;
    }

    // Arduino-ESP32 2.0.x / IDF 4.4 的旧 i2s 驱动。
    // 升级到 Arduino 3.x 后需换成 driver/i2s_std.h 的 i2s_std_config_t。
    i2s_config_t cfg = {};
    cfg.mode                = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate         = SAMPLE_RATE;
    cfg.bits_per_sample     = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format      = AIRMIC_I2S_LEFT_CHANNEL
                                  ? I2S_CHANNEL_FMT_ONLY_LEFT
                                  : I2S_CHANNEL_FMT_ONLY_RIGHT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags    = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count       = 8;
    cfg.dma_buf_len         = SAMPLES_PER_FRAME;  // 320 × 4B = 1280B / DMA buffer
    cfg.use_apll            = false;
    cfg.tx_desc_auto_clear  = false;
    cfg.fixed_mclk          = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[I2S] driver_install 失败: %d\n", static_cast<int>(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_I2S_DIN;
    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
        Serial.printf("[I2S] set_pin 失败: %d\n", static_cast<int>(err));
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    _driverReady = true;
    i2s_zero_dma_buffer(I2S_NUM_0);

    // core 0 专做采集，Arduino loop 默认在 core 1，两者互不抢占。
    const BaseType_t ok = xTaskCreatePinnedToCore(
        taskEntry, "airmic_i2s", 8192, this, 5, &_task, 0);
    if (ok != pdPASS) {
        Serial.println("[I2S] 采集任务创建失败");
        return false;
    }

    Serial.printf("[I2S] 就绪 %luHz / 32bit slot / BCLK=%d WS=%d DIN=%d / %s声道\n",
                  static_cast<unsigned long>(SAMPLE_RATE),
                  PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DIN,
                  AIRMIC_I2S_LEFT_CHANNEL ? "左" : "右");
    return true;
}

void AudioCapture::setRecording(bool on) {
    const bool was = _recording.exchange(on);
    if (on && !was && _driverReady) {
        // 待机期间 DMA 仍在跑，开始新的一段前先清掉陈旧的缓冲数据。
        i2s_zero_dma_buffer(I2S_NUM_0);
        flush();
    }
}

bool AudioCapture::pop(PcmChunk* out) {
    if (_queue == nullptr || out == nullptr) {
        return false;
    }
    return xQueueReceive(_queue, out, 0) == pdTRUE;
}

void AudioCapture::flush() {
    if (_queue == nullptr) {
        return;
    }
    PcmChunk dummy;
    while (xQueueReceive(_queue, &dummy, 0) == pdTRUE) {
        // 丢弃
    }
}

void AudioCapture::taskEntry(void* arg) {
    static_cast<AudioCapture*>(arg)->run();
}

void AudioCapture::run() {
    int32_t raw[SAMPLES_PER_FRAME];  // 1280B，任务栈 8192B 足够
    for (;;) {
        if (!_recording.load()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t got = 0;
        const esp_err_t err = i2s_read(I2S_NUM_0, raw, RAW_BYTES_PER_FRAME,
                                       &got, pdMS_TO_TICKS(100));
        if (err != ESP_OK || got != RAW_BYTES_PER_FRAME) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (!_recording.load()) {
            // 松键瞬间正在读的这一块丢掉，避免把语音段尾部送到下一段。
            continue;
        }

        PcmChunk chunk;
        chunk.len = static_cast<uint16_t>(PCM_BYTES_PER_FRAME);
        // INMP441 输出 24bit 二补码，左对齐在 32bit slot 中，低 8bit 补零。
        // 右移 16 位取高 16bit，得到 16bit PCM；直接写小端字节序。
        for (size_t i = 0; i < SAMPLES_PER_FRAME; ++i) {
            const int16_t s = static_cast<int16_t>(raw[i] >> 16);
            chunk.pcm[i * 2]     = static_cast<uint8_t>(s & 0xFF);
            chunk.pcm[i * 2 + 1] = static_cast<uint8_t>((s >> 8) & 0xFF);
        }

        // 队列满说明 Wi-Fi/发送跟不上：丢最旧的一帧、保留最新一帧，
        // 优先保证实时性（陈旧音频对 ASR 无意义），并累计丢帧数供诊断。
        if (xQueueSend(_queue, &chunk, 0) != pdTRUE) {
            PcmChunk discard;
            xQueueReceive(_queue, &discard, 0);
            xQueueSend(_queue, &chunk, 0);
            _dropped.fetch_add(1);
        }
    }
}
