#pragma once
// AirMic Node <-> AirTypeIME 帧协议 v1，见 ../AirTypeIME/docs/PROTOCOL.md
//
// 8 字节帧头，全部 Little Endian：
//   [0] magic 0xA1  [1] version 0x01  [2] kind  [3] flags(0)
//   [4..5] seq:u16  [6..7] payload_len:u16  [8..] payload
//
// 本模块刻意只用标准 C++，不依赖 Arduino / ESP-IDF，
// 因此可以在 PC 上用 [env:native] 直接跑单元测试，
// 断言与 Rust 侧 protocol.rs 的字节布局完全一致。

#include <cstddef>
#include <cstdint>
#include <string>

namespace airmic {

constexpr uint8_t MAGIC       = 0xA1;
constexpr uint8_t VERSION     = 0x01;
constexpr size_t  HEADER_LEN  = 8;
constexpr size_t  MAX_PAYLOAD = 4096;

enum Kind : uint8_t {
    K_START  = 0x01,
    K_AUDIO  = 0x02,
    K_END    = 0x03,
    K_CANCEL = 0x04,
    K_PING   = 0x05,
    K_PONG   = 0x06,
    K_STATUS = 0x81,
    K_TEXT   = 0x82,
};

struct ParsedFrame {
    uint8_t        kind;
    uint8_t        flags;
    uint16_t       seq;
    const uint8_t* payload;
    size_t         payloadLen;
};

/// 组帧。`out` 容量至少 HEADER_LEN + payloadLen，成功返回总字节数，失败返回 0。
size_t encodeFrame(uint8_t kind, uint16_t seq,
                   const uint8_t* payload, size_t payloadLen,
                   uint8_t* out, size_t outCap);

size_t encodeFrame(uint8_t kind, uint16_t seq, const std::string& payload,
                   uint8_t* out, size_t outCap);

/// 解析服务端发来的 binary 帧。payload 指向 data 内部，不拷贝。
bool decodeFrame(const uint8_t* data, size_t len, ParsedFrame* out);

/// START 帧的 JSON 元数据；codec 必须是服务端 v1 唯一支持的 pcm_s16le。
std::string startMeta(const std::string& device,
                      uint32_t sampleRate = 16000,
                      uint16_t channels = 1,
                      uint16_t bits = 16);

enum class ServerState {
    Unknown,
    Ready,
    Recognizing,
    Done,
    Cancelled,
    Error,
};

/// 从 STATUS 的 JSON 里取 state 字段。轻量解析，不引入 JSON 库。
ServerState parseStatusState(const uint8_t* json, size_t len);
const char* serverStateName(ServerState state);

}  // namespace airmic
