#include "web_upload.h"

#include <esp_heap_caps.h>

#include <string>

#include "net_client.h"
#include "protocol.h"

namespace {

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

size_t minSizeT(size_t a, size_t b) { return a < b ? a : b; }

}  // namespace

void WebUpload::begin(NetClient* net, uint16_t* seq, const String& device_id) {
    _net      = net;
    _seq      = seq;
    _deviceId = device_id;

    // 留出余量给协议栈与 String 拼接，别把 PSRAM 吃干。
    size_t cap = WEB_PCM_MAX_BYTES;
    const size_t freePsram = ESP.getFreePsram();
    if (freePsram < cap + WEB_PSRAM_MARGIN) {
        cap = (freePsram > WEB_PSRAM_MARGIN) ? (freePsram - WEB_PSRAM_MARGIN) : 0;
    }
    if (cap < 16 * 1024) {
        Serial.printf("[WEB] PSRAM 空闲仅 %luKB，网页上传不可用\n",
                      static_cast<unsigned long>(freePsram / 1024));
        return;
    }
    _pcm = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (_pcm == nullptr) {
        Serial.printf("[WEB] PSRAM 分配 %luKB 失败，网页上传不可用\n",
                      static_cast<unsigned long>(cap / 1024));
        return;
    }
    _pcmCap = cap;

    setupRoutes();
    _server.begin();
    _started = true;
    Serial.printf("[WEB] 已启动 http://<本设备IP>:%u/ （PCM 缓冲 %luKB ≈ %.1fs @16kHz/16bit）\n",
                  static_cast<unsigned>(WEB_PORT),
                  static_cast<unsigned long>(cap / 1024),
                  static_cast<double>(cap) / (SAMPLE_RATE * 2));
}

void WebUpload::loop() {
    if (!_started) {
        return;
    }
    _server.handleClient();

    if (_state == State::Receiving) {
        // 看门狗：客户端半途断开时 _parseRequest 返回 false，POST 回调根本不执行，
        // 状态会永远卡在 Receiving 并把 PTT 一起锁死。
        if (millis() - _lastActivityMs > WEB_RECEIVE_TIMEOUT_MS) {
            Serial.println("[WEB] 接收超时，放弃本次上传");
            fail("receive timeout");
        }
        return;
    }
    if (_state != State::Replaying) {
        return;
    }

    const uint32_t now = millis();
    if (now - _replayNextMs < WEB_REPLAY_GAP_MS) {
        return;
    }
    _replayNextMs = now;

    if (_net == nullptr || !_net->connected()) {
        Serial.println("[WEB] 重放期间 WebSocket 断开，丢弃剩余音频");
        fail("ws disconnected");
        return;
    }

    if (_replayOffset >= _pcmLen) {
        const uint16_t seq = (*_seq)++;
        _net->send(airmic::K_END, seq, nullptr, 0);
        Serial.printf("[WEB] 重放完成：%lu 字节 / %lu 帧\n",
                      static_cast<unsigned long>(_pcmLen),
                      static_cast<unsigned long>(_framesSent));
        _state         = State::Idle;
        _pcmLen        = 0;
        _replayOffset  = 0;
        _framesSent    = 0;
        return;
    }

    uint8_t frame[PCM_BYTES_PER_FRAME];
    const size_t remain = _pcmLen - _replayOffset;
    const size_t take   = minSizeT(remain, sizeof(frame));
    memcpy(frame, _pcm + _replayOffset, take);
    if (take < sizeof(frame)) {
        // 尾帧补零到整 20ms，避免服务端拿到奇数/半帧。
        memset(frame + take, 0, sizeof(frame) - take);
    }
    const uint16_t seq = (*_seq)++;
    if (!_net->send(airmic::K_AUDIO, seq, frame, sizeof(frame))) {
        Serial.println("[WEB] AUDIO 发送失败，终止重放");
        fail("send failed");
        return;
    }
    _replayOffset += take;
    ++_framesSent;
}

bool WebUpload::consumeError() {
    if (!_error) {
        return false;
    }
    _error = false;
    return true;
}

const char* WebUpload::stateName() const {
    switch (_state) {
        case State::Receiving: return "receiving";
        case State::Replaying: return "replaying";
        default:               return "idle";
    }
}

// ---------------------------------------------------------------------------
// 路由
// ---------------------------------------------------------------------------

void WebUpload::setupRoutes() {
    _server.on("/", HTTP_GET, [this]() { handleRoot(); });
    _server.on("/api/state", HTTP_GET, [this]() { handleState(); });
    // 带第 4 个参数才会走文件上传通道（HTTPUpload），否则 POST 会被当成普通表单。
    _server.on("/upload", HTTP_POST,
               [this]() { handleUploadDone(); },
               [this]() { handleUploadChunk(); });
    _server.onNotFound([this]() { handleNotFound(); });
}

void WebUpload::handleNotFound() {
    _server.send(404, "text/plain; charset=utf-8", "Not Found");
}

void WebUpload::sendJson(int code, const String& body) {
    _server.send(code, "application/json; charset=utf-8", body);
}

void WebUpload::handleState() {
    String body = "{";
    body += "\"started\":";
    body += _started ? "true" : "false";
    body += ",\"ws\":";
    body += (_net != nullptr && _net->connected()) ? "true" : "false";
    body += ",\"state\":\"";
    body += stateName();
    body += "\",\"pcm\":";
    body += String(static_cast<unsigned long>(_pcmLen));
    body += ",\"cap\":";
    body += String(static_cast<unsigned long>(_pcmCap));
    body += ",\"frames\":";
    body += String(static_cast<unsigned long>(_framesSent));
    body += ",\"freeHeap\":";
    body += String(static_cast<unsigned long>(ESP.getFreeHeap()));
    body += "}";
    sendJson(200, body);
}

// ---------------------------------------------------------------------------
// 上传与 WAV 解析
// ---------------------------------------------------------------------------

void WebUpload::resetReceive() {
    _pcmLen   = 0;
    _overflow = false;
    _aborted  = false;
    _wav      = WavState::Riff;
    _hdrIdx   = 0;
    _fmtIdx   = 0;
    _chunkLeft = 0;
    _fmtOk    = false;
    clearError();
    _lastActivityMs = millis();
}

void WebUpload::handleUploadChunk() {
    HTTPUpload& up = _server.upload();
    _lastActivityMs = millis();

    switch (up.status) {
        case UPLOAD_FILE_START: {
            resetReceive();
            if (_state == State::Replaying) {
                _overflow = true;  // 复用 overflow 作为「拒绝本次上传」标记
                Serial.println("[WEB] 上一段仍在重放，拒绝新上传");
                break;
            }
            _state = State::Receiving;
            // multipart 外层还含 boundary 与 part 头，粗略放宽 WEB_WAV_OVERHEAD_MAX。
            if (up.totalSize > _pcmCap + WEB_WAV_OVERHEAD_MAX) {
                _overflow = true;
                Serial.printf("[WEB] 文件过大：%u 字节（上限约 %lu）\n",
                              static_cast<unsigned>(up.totalSize),
                              static_cast<unsigned long>(_pcmCap));
            }
            break;
        }

        case UPLOAD_FILE_WRITE:
            // 注意：只有 WRITE 才带数据。收尾前库会再调一次 WRITE 把最后不满
            // 1436B 的残留冲出来，随后才是 END，所以 END 里不能再喂一次。
            if (!_overflow) {
                feed(up.buf, up.currentSize);
            }
            break;

        case UPLOAD_FILE_END:
            break;

        case UPLOAD_FILE_ABORTED:
            _aborted = true;
            Serial.println("[WEB] 上传被客户端中止");
            break;
    }
}

void WebUpload::handleUploadDone() {
    if (!_started) {
        return;
    }

    if (_aborted) {
        fail("upload aborted");
        sendJson(500, "{\"ok\":false,\"error\":\"upload aborted\"}");
        return;
    }

    if (_error) {
        String body = "{\"ok\":false,\"error\":\"";
        body += _errorMsg;
        body += "\"}";
        fail(nullptr);  // 复位状态与缓冲
        sendJson(400, body);
        return;
    }

    if (_state != State::Receiving) {
        sendJson(409, "{\"ok\":false,\"error\":\"not receiving\"}");
        return;
    }

    if (_pcmLen < 2000) {  // 不足 60ms，识别不出东西
        fail("audio too short");
        sendJson(400, "{\"ok\":false,\"error\":\"audio too short\"}");
        return;
    }

    if (_net == nullptr || !_net->connected()) {
        fail("websocket not connected");
        sendJson(503, "{\"ok\":false,\"error\":\"websocket not connected\"}");
        return;
    }

    const std::string meta = airmic::startMeta(_deviceId.c_str(), SAMPLE_RATE, 1, 16);
    const uint16_t seq = (*_seq)++;
    if (!_net->send(airmic::K_START, seq,
                    reinterpret_cast<const uint8_t*>(meta.data()), meta.size())) {
        fail("START send failed");
        sendJson(503, "{\"ok\":false,\"error\":\"START send failed\"}");
        return;
    }

    _replayOffset = 0;
    _framesSent   = 0;
    _replayNextMs = millis();
    _state        = State::Replaying;

    Serial.printf("[WEB] 收到 PCM %lu 字节（%.1fs），开始重放\n",
                  static_cast<unsigned long>(_pcmLen),
                  static_cast<double>(_pcmLen) / (SAMPLE_RATE * 2));

    String body = "{\"ok\":true,\"bytes\":";
    body += String(static_cast<unsigned long>(_pcmLen));
    body += ",\"seconds\":";
    body += String(static_cast<double>(_pcmLen) / (SAMPLE_RATE * 2), 1);
    body += "}";
    sendJson(200, body);
}

void WebUpload::fail(const char* msg) {
    _error = true;
    _errorMsg = (msg != nullptr) ? msg : "";
    _state        = State::Idle;
    _pcmLen       = 0;
    _replayOffset = 0;
    _framesSent   = 0;
    if (msg != nullptr) {
        Serial.printf("[WEB] 错误：%s\n", msg);
    }
}

void WebUpload::clearError() {
    _error = false;
    _errorMsg = "";
}

/// 增量解析 RIFF/WAVE：只认 PCM / 单声道 / 16bit / 16kHz。
void WebUpload::feed(const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    size_t left = len;

    while (left > 0 && _wav != WavState::Done && _wav != WavState::Failed) {
        switch (_wav) {
            case WavState::Riff: {
                const size_t take = minSizeT(left, sizeof(_hdr) - _hdrIdx);
                memcpy(_hdr + _hdrIdx, p, take);
                _hdrIdx += take;
                p += take;
                left -= take;
                if (_hdrIdx == sizeof(_hdr)) {
                    if (memcmp(_hdr, "RIFF", 4) != 0 || memcmp(_hdr + 8, "WAVE", 4) != 0) {
                        fail("不是 RIFF/WAVE 文件");
                    } else {
                        _wav    = WavState::Header;
                        _hdrIdx = 0;
                    }
                }
                break;
            }

            case WavState::Header: {
                const size_t take = minSizeT(left, 8 - _hdrIdx);
                memcpy(_hdr + _hdrIdx, p, take);
                _hdrIdx += take;
                p += take;
                left -= take;
                if (_hdrIdx == 8) {
                    const uint32_t sz = rd32(_hdr + 4);
                    _hdrIdx = 0;
                    if (memcmp(_hdr, "fmt ", 4) == 0) {
                        if (sz != sizeof(_fmt)) {
                            fail("fmt 块长度不是 16，只支持标准 PCM");
                            break;
                        }
                        _chunkLeft = sz;
                        _fmtIdx    = 0;
                        _wav       = WavState::Fmt;
                    } else if (memcmp(_hdr, "data", 4) == 0) {
                        if (!_fmtOk) {
                            fail("data 块出现在 fmt 之前");
                            break;
                        }
                        _chunkLeft = sz;
                        _wav       = WavState::Data;
                    } else {
                        // LIST / fact 等无关块：整体跳过。
                        _chunkLeft = sz;
                        _wav       = WavState::Skip;
                    }
                }
                break;
            }

            case WavState::Fmt: {
                const size_t take = minSizeT(left, _chunkLeft - _fmtIdx);
                memcpy(_fmt + _fmtIdx, p, take);
                _fmtIdx += take;
                p += take;
                left -= take;
                if (_fmtIdx == sizeof(_fmt)) {
                    const uint16_t format = rd16(_fmt);
                    const uint16_t ch     = rd16(_fmt + 2);
                    const uint32_t rate   = rd32(_fmt + 4);
                    const uint16_t bits   = rd16(_fmt + 14);
                    if (format != 1) {
                        fail("只支持 PCM（format != 1）");
                    } else if (ch != 1) {
                        fail("只支持单声道");
                    } else if (rate != SAMPLE_RATE) {
                        fail("采样率必须是 16000");
                    } else if (bits != 16) {
                        fail("位深必须是 16bit");
                    } else {
                        _fmtOk = true;
                        _wav   = WavState::Header;
                        _hdrIdx = 0;
                    }
                }
                break;
            }

            case WavState::Data: {
                const size_t take = minSizeT(left, _chunkLeft);
                appendPcm(p, take);
                p += take;
                left -= take;
                _chunkLeft -= take;
                if (_chunkLeft == 0) {
                    _wav = WavState::Done;
                }
                break;
            }

            case WavState::Skip: {
                const size_t take = minSizeT(left, _chunkLeft);
                p += take;
                left -= take;
                _chunkLeft -= take;
                if (_chunkLeft == 0) {
                    _wav    = WavState::Header;
                    _hdrIdx = 0;
                }
                break;
            }

            default:
                return;
        }
    }
}

void WebUpload::appendPcm(const uint8_t* data, size_t len) {
    if (_overflow) {
        return;
    }
    if (_pcmLen + len > _pcmCap) {
        _overflow = true;
        Serial.printf("[WEB] PCM 超出缓冲（%lu / %lu 字节），后续数据丢弃\n",
                      static_cast<unsigned long>(_pcmLen + len),
                      static_cast<unsigned long>(_pcmCap));
        return;
    }
    memcpy(_pcm + _pcmLen, data, len);
    _pcmLen += len;
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------

namespace {

const char* const PAGE_STYLE =
    "*{box-sizing:border-box}"
    "body{margin:0;padding:20px 14px;background:#f4f5f7;color:#1c1e21;"
    "font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}"
    "main{max-width:440px;margin:0 auto}"
    "h1{font-size:20px;margin:0 0 6px}"
    ".tip{font-size:13px;color:#5f6368;line-height:1.6;margin:0 0 14px}"
    "button{width:100%;padding:13px;font-size:16px;border:0;border-radius:8px;"
    "background:#1a73e8;color:#fff;font-family:inherit;cursor:pointer}"
    "button.rec{background:#c5221f}"
    "button:disabled{opacity:.5;cursor:default}"
    ".meta{font-size:13px;color:#3c4043;margin:12px 0 6px}"
    ".msg{padding:10px;border-radius:6px;font-size:13px;line-height:1.5;display:none}"
    ".ok{background:#e6f4ea;color:#137333;display:block}"
    ".err{background:#fce8e6;color:#c5221f;display:block}"
    ".log{background:#fff;border-radius:6px;padding:10px;font-size:12px;color:#5f6368;"
    "font-family:ui-monospace,Consolas,monospace;white-space:pre-wrap;"
    "word-break:break-all;max-height:180px;overflow:auto}";

const char* const PAGE_JS =
    "var btn=document.getElementById('btn'),timerEl=document.getElementById('timer'),"
    "sizeEl=document.getElementById('size'),wsEl=document.getElementById('ws'),"
    "msgEl=document.getElementById('msg'),logEl=document.getElementById('log');"
    "var ctx=null,stream=null,source=null,proc=null,chunks=[],recording=false,t0=0,timerId=null;"
    "function log(s){logEl.textContent+=s+'\\n';logEl.scrollTop=logEl.scrollHeight;}"
    "function msg(s,bad){msgEl.textContent=s;msgEl.className='msg '+(bad?'err':'ok');}"
    "function fmt(n){return (n/1024).toFixed(1)+' KB';}"
    "function resetBtn(){btn.disabled=false;btn.textContent='开始录音';btn.className='';}"
    "async function loadState(){"
    "try{var r=await fetch('/api/state');var j=await r.json();"
    "wsEl.textContent=j.ws?'已连接':'未连接';"
    "if(!j.started){msg('设备网页服务未启动',true);btn.disabled=true;}"
    "else if(!j.ws){msg('WebSocket 未连接：确认 PC 上 AirType 已激活、地址与防火墙正确',true);}"
    "}catch(e){wsEl.textContent='未知';}}"
    "async function start(){if(recording)return;"
    "try{stream=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,"
    "echoCancellation:true,noiseSuppression:true,autoGainControl:true}});"
    "}catch(e){msg('拿不到麦克风权限：'+e.message,true);return;}"
    "var AC=window.AudioContext||window.webkitAudioContext;"
    "if(!AC){msg('浏览器不支持 Web Audio',true);return;}"
    "try{ctx=new AC({sampleRate:16000});}catch(e){ctx=new AC();}"
    "if(ctx.state==='suspended'){ctx.resume();}"
    "if(ctx.sampleRate!==16000){log('采样率 '+ctx.sampleRate+' Hz，停止后重采样到 16k');}"
    "chunks=[];sizeEl.textContent=fmt(0);"
    "source=ctx.createMediaStreamSource(stream);"
    "proc=ctx.createScriptProcessor(4096,1,1);"
    "proc.onaudioprocess=function(e){var d=e.inputBuffer.getChannelData(0);"
    "var c=new Float32Array(d.length);c.set(d);chunks.push(c);"
    "var n=0;for(var i=0;i<chunks.length;i++)n+=chunks[i].length;"
    "sizeEl.textContent=fmt(n*2);};"
    "source.connect(proc);proc.connect(ctx.destination);"
    "recording=true;t0=performance.now();"
    "btn.textContent='停止并上传';btn.className='rec';"
    "timerId=setInterval(function(){timerEl.textContent="
    "((performance.now()-t0)/1000).toFixed(1)+' s';},100);msg('');}"
    "function stop(){if(!recording)return;recording=false;clearInterval(timerId);"
    "try{source.disconnect();proc.disconnect();}catch(e){}"
    "var tracks=stream?stream.getTracks():[];"
    "for(var i=0;i<tracks.length;i++)tracks[i].stop();"
    "var total=0,i;for(i=0;i<chunks.length;i++)total+=chunks[i].length;"
    "var merged=new Float32Array(total),off=0;"
    "for(i=0;i<chunks.length;i++){merged.set(chunks[i],off);off+=chunks[i].length;}"
    "chunks=[];"
    "if(total<800){msg('录音太短',true);resetBtn();return;}"
    "var done=function(samples){upload(makeWav(samples,16000));};"
    "if(ctx.sampleRate===16000){done(merged);return;}"
    "var len=Math.max(1,Math.round(total*16000/ctx.sampleRate));"
    "var OC=window.OfflineAudioContext||window.webkitOfflineAudioContext;"
    "var oc=new OC(1,len,16000);"
    "var buf=oc.createBuffer(1,total,ctx.sampleRate);buf.copyToChannel(merged,0);"
    "var bs=oc.createBufferSource();bs.buffer=buf;bs.connect(oc.destination);bs.start();"
    "oc.startRendering().then(function(r){done(r.getChannelData(0));})"
    ".catch(function(e){msg('重采样失败：'+e.message,true);resetBtn();});}"
    "function makeWav(samples,rate){var n=samples.length,buf=new ArrayBuffer(44+n*2),"
    "v=new DataView(buf),i;"
    "function ws(o,s){for(var k=0;k<s.length;k++)v.setUint8(o+k,s.charCodeAt(k));}"
    "ws(0,'RIFF');v.setUint32(4,36+n*2,true);ws(8,'WAVE');ws(12,'fmt ');"
    "v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);"
    "v.setUint32(24,rate,true);v.setUint32(28,rate*2,true);"
    "v.setUint16(32,2,true);v.setUint16(34,16,true);"
    "ws(36,'data');v.setUint32(40,n*2,true);"
    "for(i=0;i<n;i++){var s=Math.max(-1,Math.min(1,samples[i]));"
    "v.setInt16(44+i*2,s<0?s*32768:s*32767,true);}"
    "return new Blob([buf],{type:'application/octet-stream'});}"
    "async function upload(wav){msg('上传 '+fmt(wav.size)+' …',false);"
    "try{var fd=new FormData();fd.append('file',wav,'audio.wav');"
    "var r=await fetch('/upload',{method:'POST',body:fd});"
    "var j=await r.json();"
    "if(j.ok){msg('已接收 '+j.bytes+' 字节（'+j.seconds+' s），识别结果稍后写入光标。',false);}"
    "else{msg('上传被拒：'+(j.error||('HTTP '+r.status)),true);}"
    "}catch(e){msg('网络错误：'+e.message,true);}"
    "resetBtn();loadState();}"
    "btn.addEventListener('click',function(){if(recording){stop();}else{start();}});"
    "loadState();";

}  // namespace

void WebUpload::handleRoot() {
    String html;
    html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>AirMic 语音上传</title><style>");
    html += PAGE_STYLE;
    html += F("</style></head><body><main>"
              "<h1>AirMic 语音上传</h1>"
              "<p class=\"tip\">点「开始录音」说一段话，松手后自动上传；"
              "识别结果由 AirType 写到当前光标处。请先在目标窗口切到 AirType 输入法。</p>"
              "<button id=\"btn\">开始录音</button>"
              "<div class=\"meta\"><span id=\"timer\">0.0 s</span> · "
              "<span id=\"size\">0 KB</span> · WS <span id=\"ws\">?</span></div>"
              "<div id=\"msg\" class=\"msg\"></div>"
              "<div id=\"log\" class=\"log\"></div>"
              "<script>");
    html += PAGE_JS;
    html += F("</script></main></body></html>");
    _server.send(200, "text/html; charset=utf-8", html);
}
