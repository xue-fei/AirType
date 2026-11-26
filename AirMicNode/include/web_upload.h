#pragma once
// 网页上传音频：没有麦克风 / PTT 硬件时的替代输入路径。
//
//   GET  /        录音页（Web Audio 采 16k/16bit/mono，封 WAV 上传）
//   POST /upload  multipart 文件字段 "file"，边收边剥 WAV 头，PCM 存 PSRAM
//   GET  /api/state  JSON：WS 连接、当前状态、已缓冲字节
//
// 收完后在 loop() 里按 WEB_REPLAY_GAP_MS 的节奏补发成协议帧：
//   START(device=airmic-web) -> AUDIO(640B S16LE) * N -> END
// 之后的识别与上屏完全走 daemon / TSF 已有的链路，本模块不碰。
//
// 为什么先缓冲再重放：WebServer 的 handleClient() 是阻塞的，边收边发会
// 让 WebSocketsClient 得不到 loop() 而掉线；PSRAM 有 2MB，够用。

#include <Arduino.h>
#include <WebServer.h>

#include <stddef.h>
#include <stdint.h>

#include "airmic_config.h"

class NetClient;

class WebUpload {
public:
    /// net 用于取连接状态与发帧；seq 指向全局连续递增的帧序号；
    /// device_id 放进 START 元数据，便于服务端区分来源。
    void begin(NetClient* net, uint16_t* seq, const String& device_id);
    void loop();

    /// 正在接收或重放：此时应忽略 PTT，避免两路音频互相插队。
    bool busy() const { return _state != State::Idle; }
    /// 取走并清除错误标记（供主循环点亮错误灯）。
    bool consumeError();

    // ---- 给页面 / 串口用 ----
    bool    started()      const { return _started; }
    size_t  pcmBytes()     const { return _pcmLen; }
    size_t  pcmCapacity()  const { return _pcmCap; }
    uint32_t framesSent()  const { return _framesSent; }
    const char* stateName() const;

private:
    enum class State { Idle, Receiving, Replaying };
    enum class WavState { Riff, Header, Fmt, Skip, Data, Done, Failed };

    void setupRoutes();
    void handleRoot();
    void handleState();
    void handleNotFound();
    void handleUploadChunk();
    void handleUploadDone();
    void sendJson(int code, const String& body);

    /// 重置一次接收（新文件开始）。
    void resetReceive();
    /// 喂 WAV 字节，内部完成 RIFF/fmt/data 解析与 PCM 落缓冲。
    void feed(const uint8_t* data, size_t len);
    void appendPcm(const uint8_t* data, size_t len);
    void fail(const char* msg);
    void clearError();

    NetClient* _net   = nullptr;
    uint16_t*  _seq   = nullptr;
    String     _deviceId;

    WebServer  _server{WEB_PORT};
    bool       _started = false;
    State      _state   = State::Idle;
    uint32_t   _lastActivityMs = 0;

    // ---- 接收缓冲（PSRAM）----
    uint8_t*   _pcm     = nullptr;
    size_t     _pcmCap  = 0;
    size_t     _pcmLen  = 0;
    bool       _overflow = false;

    // ---- WAV 解析 ----
    WavState   _wav     = WavState::Riff;
    uint8_t    _hdr[12] = {0};
    size_t     _hdrIdx  = 0;
    uint8_t    _fmt[16] = {0};
    size_t     _fmtIdx  = 0;
    uint32_t   _chunkLeft = 0;
    bool       _fmtOk   = false;
    bool       _aborted = false;

    // ---- 重放 ----
    size_t     _replayOffset = 0;
    uint32_t   _replayNextMs = 0;
    uint32_t   _framesSent   = 0;

    bool       _error = false;
    String     _errorMsg;
};
