#include <unity.h>

#include <cstring>
#include <string>

#include "protocol.h"

using namespace airmic;

void setUp() {}
void tearDown() {}

// 与 AirTypeIME/crates/airtype-core/src/protocol.rs 的
// `header_layout_is_stable` 断言逐字节对齐。
void test_header_layout_matches_rust_side() {
    uint8_t out[16] = {0};
    const uint8_t payload[2] = {0x10, 0x20};
    const size_t n = encodeFrame(K_AUDIO, 0x0102, payload, 2, out, sizeof(out));

    const uint8_t expected[10] = {
        0xA1, 0x01, 0x02, 0x00, 0x02, 0x01, 0x02, 0x00, 0x10, 0x20
    };
    TEST_ASSERT_EQUAL_size_t(10, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 10);
}

void test_empty_frame_is_header_only() {
    uint8_t out[8] = {0};
    TEST_ASSERT_EQUAL_size_t(8, encodeFrame(K_END, 0, nullptr, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(K_END, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0, out[7]);
}

void test_seq_wraparound_is_little_endian() {
    uint8_t out[8] = {0};
    const size_t n = encodeFrame(K_PING, 0xFFFF, nullptr, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[5]);
}

void test_20ms_pcm_frame_size() {
    // 320 samples × 2 bytes = 640B payload + 8B header = 648B，帧长为偶数。
    uint8_t out[8 + 640] = {0};
    uint8_t pcm[640] = {0};
    pcm[0] = 0x34;
    pcm[1] = 0x12;  // S16LE 的 0x1234
    const size_t n = encodeFrame(K_AUDIO, 7, pcm, sizeof(pcm), out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(648, n);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[6]);  // 640 = 0x0280
    TEST_ASSERT_EQUAL_HEX8(0x02, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0x07, out[4]);  // seq = 7
    TEST_ASSERT_EQUAL_HEX8(0x34, out[8]);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[9]);
}

void test_decode_round_trip() {
    uint8_t out[8 + 640] = {0};
    uint8_t pcm[640] = {0};
    for (size_t i = 0; i < sizeof(pcm); ++i) {
        pcm[i] = static_cast<uint8_t>(i);
    }
    const size_t n = encodeFrame(K_AUDIO, 0xBEEF, pcm, sizeof(pcm), out, sizeof(out));

    ParsedFrame f{};
    TEST_ASSERT_TRUE(decodeFrame(out, n, &f));
    TEST_ASSERT_EQUAL_HEX8(K_AUDIO, f.kind);
    TEST_ASSERT_EQUAL_HEX8(0, f.flags);
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, f.seq);
    TEST_ASSERT_EQUAL_size_t(640, f.payloadLen);
    TEST_ASSERT_EQUAL_MEMORY(pcm, f.payload, sizeof(pcm));
}

void test_decode_rejects_corrupt_frames() {
    ParsedFrame f{};
    uint8_t frame[8] = {0};
    TEST_ASSERT_EQUAL_size_t(8, encodeFrame(K_PING, 0, nullptr, 0, frame, sizeof(frame)));

    TEST_ASSERT_FALSE(decodeFrame(frame, 2, &f));  // 太短
    frame[0] = 0x00;
    TEST_ASSERT_FALSE(decodeFrame(frame, 8, &f));  // magic 错
    frame[0] = MAGIC;
    frame[1] = 0x02;
    TEST_ASSERT_FALSE(decodeFrame(frame, 8, &f));  // 版本错
    frame[1] = VERSION;
    frame[6] = 7;
    TEST_ASSERT_FALSE(decodeFrame(frame, 8, &f));  // 长度不符
    frame[6] = 0;
    frame[7] = 0x11;  // declared = 4352 > MAX_PAYLOAD
    TEST_ASSERT_FALSE(decodeFrame(frame, 8, &f));
}

void test_encode_rejects_oversize_and_small_buffer() {
    uint8_t out[16] = {0};
    uint8_t big[4097] = {0};
    TEST_ASSERT_EQUAL_size_t(0, encodeFrame(K_AUDIO, 0, big, sizeof(big), out, sizeof(out)));

    uint8_t two[2] = {0xAA, 0xBB};
    uint8_t small[9] = {0};  // 需要 8+2=10 字节
    TEST_ASSERT_EQUAL_size_t(0, encodeFrame(K_AUDIO, 0, two, 2, small, sizeof(small)));
}

void test_start_meta_declares_pcm_s16le() {
    const std::string json = startMeta("airmic-01");
    TEST_ASSERT_TRUE(json.find("\"device\":\"airmic-01\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"codec\":\"pcm_s16le\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"sample_rate\":16000") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"channels\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"bits_per_sample\":16") != std::string::npos);
    // 非法字符必须被替换，保证 JSON 不被破坏
    TEST_ASSERT_TRUE(startMeta("air\"mic").find("air_mic") != std::string::npos);
}

void test_parse_status_state() {
    const char* cases[] = {
        "{\"state\":\"ready\",\"text\":\"\",\"message\":\"\"}",
        "{ \"state\" : \"recognizing\" }",
        "{\"state\":\"done\",\"text\":\"你好\",\"message\":\"\"}",
        "{\"state\":\"cancelled\"}",
        "{\"state\":\"error\",\"message\":\"bad magic\"}",
    };
    const ServerState expected[] = {
        ServerState::Ready, ServerState::Recognizing, ServerState::Done,
        ServerState::Cancelled, ServerState::Error,
    };
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(expected[i]),
            static_cast<int>(parseStatusState(
                reinterpret_cast<const uint8_t*>(cases[i]), std::strlen(cases[i]))));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ServerState::Unknown),
                          static_cast<int>(parseStatusState(nullptr, 0)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_header_layout_matches_rust_side);
    RUN_TEST(test_empty_frame_is_header_only);
    RUN_TEST(test_seq_wraparound_is_little_endian);
    RUN_TEST(test_20ms_pcm_frame_size);
    RUN_TEST(test_decode_round_trip);
    RUN_TEST(test_decode_rejects_corrupt_frames);
    RUN_TEST(test_encode_rejects_oversize_and_small_buffer);
    RUN_TEST(test_start_meta_declares_pcm_s16le);
    RUN_TEST(test_parse_status_state);
    return UNITY_END();
}
