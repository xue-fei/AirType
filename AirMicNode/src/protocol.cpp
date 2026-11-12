#include "protocol.h"

#include <cstring>

namespace airmic {

size_t encodeFrame(uint8_t kind, uint16_t seq,
                   const uint8_t* payload, size_t payloadLen,
                   uint8_t* out, size_t outCap) {
    if (out == nullptr || payloadLen > MAX_PAYLOAD) {
        return 0;
    }
    if (payloadLen > 0 && payload == nullptr) {
        return 0;
    }
    const size_t total = HEADER_LEN + payloadLen;
    if (outCap < total) {
        return 0;
    }

    out[0] = MAGIC;
    out[1] = VERSION;
    out[2] = kind;
    out[3] = 0;  // flags: v1 固定 0
    out[4] = static_cast<uint8_t>(seq & 0xFF);
    out[5] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>(payloadLen & 0xFF);
    out[7] = static_cast<uint8_t>((payloadLen >> 8) & 0xFF);
    if (payloadLen > 0) {
        std::memcpy(out + HEADER_LEN, payload, payloadLen);
    }
    return total;
}

size_t encodeFrame(uint8_t kind, uint16_t seq, const std::string& payload,
                   uint8_t* out, size_t outCap) {
    return encodeFrame(kind, seq,
                       reinterpret_cast<const uint8_t*>(payload.data()),
                       payload.size(), out, outCap);
}

bool decodeFrame(const uint8_t* data, size_t len, ParsedFrame* out) {
    if (data == nullptr || out == nullptr || len < HEADER_LEN) {
        return false;
    }
    if (data[0] != MAGIC || data[1] != VERSION) {
        return false;
    }
    const size_t declared = static_cast<size_t>(data[6]) |
                            (static_cast<size_t>(data[7]) << 8);
    if (declared > MAX_PAYLOAD || declared != len - HEADER_LEN) {
        return false;
    }
    out->kind       = data[2];
    out->flags      = data[3];
    out->seq        = static_cast<uint16_t>(data[4]) |
                      (static_cast<uint16_t>(data[5]) << 8);
    out->payload    = data + HEADER_LEN;
    out->payloadLen = declared;
    return true;
}

std::string startMeta(const std::string& device, uint32_t sampleRate,
                      uint16_t channels, uint16_t bits) {
    // device 由 MAC 或配置生成，只允许 ASCII 字母数字与 '-'/'_'，无需转义。
    std::string safe;
    safe.reserve(device.size());
    for (char c : device) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe.push_back(ok ? c : '_');
    }
    if (safe.empty()) {
        safe = "unknown";
    }

    std::string json = "{\"device\":\"" + safe + "\"";
    json += ",\"codec\":\"pcm_s16le\"";
    json += ",\"sample_rate\":" + std::to_string(sampleRate);
    json += ",\"channels\":" + std::to_string(channels);
    json += ",\"bits_per_sample\":" + std::to_string(bits);
    json += "}";
    return json;
}

ServerState parseStatusState(const uint8_t* json, size_t len) {
    if (json == nullptr || len == 0) {
        return ServerState::Unknown;
    }
    const std::string s(reinterpret_cast<const char*>(json), len);
    size_t p = s.find("\"state\"");
    if (p == std::string::npos) {
        return ServerState::Unknown;
    }
    p = s.find(':', p);
    if (p == std::string::npos) {
        return ServerState::Unknown;
    }
    p = s.find('"', p);
    if (p == std::string::npos) {
        return ServerState::Unknown;
    }
    const size_t end = s.find('"', p + 1);
    if (end == std::string::npos) {
        return ServerState::Unknown;
    }
    const std::string v = s.substr(p + 1, end - p - 1);
    if (v == "ready")       return ServerState::Ready;
    if (v == "recognizing") return ServerState::Recognizing;
    if (v == "done")        return ServerState::Done;
    if (v == "cancelled")   return ServerState::Cancelled;
    if (v == "error")       return ServerState::Error;
    return ServerState::Unknown;
}

const char* serverStateName(ServerState state) {
    switch (state) {
        case ServerState::Ready:       return "ready";
        case ServerState::Recognizing: return "recognizing";
        case ServerState::Done:        return "done";
        case ServerState::Cancelled:   return "cancelled";
        case ServerState::Error:       return "error";
        default:                       return "unknown";
    }
}

}  // namespace airmic
