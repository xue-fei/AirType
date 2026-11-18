#include "provisioning.h"

#include "credential_store.h"

namespace {

const char* const SERIAL_USAGE =
    "SSID:密码  或  {\"ssid\":\"..\",\"pass\":\"..\"}"
    "（JSON 可加 \"host\":\"..\",\"port\":..）";

String portToString(uint16_t port) {
    return String(static_cast<unsigned>(port));
}

}  // namespace

Provisioning::Result Provisioning::run(const String& apName, StatusLed& led) {
    Serial.println();
    Serial.println("=== AirMic 配网模式 ===");
    Serial.printf("1) 连上热点 %s，打开 http://192.168.4.1/\n", apName.c_str());
    Serial.printf("2) 或在本串口发一行：%s\n", SERIAL_USAGE);

    led.set(LedState::Provisioning);
    startAp(apName);
    setupRoutes();
    refreshScan();
    _server.begin();
    _startMs = millis();

    while (true) {
        _server.handleClient();

        while (Serial.available() > 0) {
            airmic::WifiCredentials c;
            if (serialFeed(static_cast<uint8_t>(Serial.read()), &c)) {
                Serial.printf("[配网] 串口收到 SSID=%s host=%s port=%u\n",
                              c.ssid.c_str(),
                              c.hasHostPort() ? c.host.c_str() : "(默认)",
                              static_cast<unsigned>(c.port));
                if (CredentialStore::save(c)) {
                    Serial.println("[配网] 已写入 NVS，重启...");
                    _saved = true;
                } else {
                    Serial.println("[配网] 写入 NVS 失败");
                }
                break;
            }
        }

        led.update();

        if (_saved) {
            // 留一点时间把响应发出去，再让调用方重启
            delay(600);
            stopAp();
            return Result::Saved;
        }
        if (millis() - _startMs >= PROVISION_TIMEOUT_MS) {
            Serial.println("[配网] 超时，退出配网模式");
            stopAp();
            return Result::Timeout;
        }
        delay(1);  // 让出 CPU 给 Wi-Fi / TCP 任务
    }
}

bool Provisioning::serialFeed(uint8_t byte, airmic::WifiCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    if (byte == '\r') {
        return false;  // 等 '\n' 作为行结束
    }
    if (byte == '\n') {
        if (_lineBuf.isEmpty()) {
            return false;
        }
        const String line = _lineBuf;
        _lineBuf = "";
        airmic::WifiCredentials c;
        bool ok = airmic::parseJsonCredentials(line.c_str(), &c);
        if (!ok) {
            ok = airmic::parseSimpleCredentials(line.c_str(), &c);
        }
        if (!ok) {
            Serial.printf("[配网] 格式不对，请用：%s\n", SERIAL_USAGE);
            return false;
        }
        *out = c;
        return true;
    }
    if (_lineBuf.length() < 128) {
        _lineBuf += static_cast<char>(byte);
    }
    return false;
}

void Provisioning::startAp(const String& apName) {
    // AP_STA 而不是 AP：扫附近热点时 STA 射频仍然可用，扫完热点不会掉
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(apName.c_str(), nullptr)) {
        Serial.println("[配网] SoftAP 启动失败");
        return;
    }
    Serial.printf("[配网] 热点 %s / http://%s/\n",
                  apName.c_str(), WiFi.softAPIP().toString().c_str());
}

void Provisioning::stopAp() {
    _server.stop();
    WiFi.softAPdisconnect(true);
}

void Provisioning::setupRoutes() {
    _server.on("/", HTTP_GET, [this]() { handleRoot(); });
    _server.on("/save", HTTP_POST, [this]() { handleSave(); });
    _server.on("/rescan", HTTP_GET, [this]() { handleRescan(); });
    _server.onNotFound([this]() { handleNotFound(); });
}

void Provisioning::handleRoot() {
    String host;
    uint16_t port = 0;
    loadTarget(&host, &port);
    _server.send(200, "text/html; charset=utf-8",
                 renderPage(nullptr, false, host, portToString(port)));
}

void Provisioning::handleSave() {
    // 注意：WebServer::arg() 已经做过 URL 解码（'+' -> 空格、%XX -> 字节），
    // 这里不要再调 airmic::urlDecode，否则会二次解码。
    const String hostArg = _server.arg("host");
    const String portArg = _server.arg("port");

    airmic::WifiCredentials c;
    c.ssid = _server.arg("ssid").c_str();
    c.pass = _server.arg("pass").c_str();
    c.host = hostArg.c_str();

    uint16_t port = 0;
    if (!airmic::parsePortText(portArg.c_str(), &port)) {
        _server.send(400, "text/html; charset=utf-8",
                    renderPage("端口必须是 1–65535 的数字，或留空用默认端口。",
                               true, hostArg, portArg));
        return;
    }
    c.port = port;

    if (!c.valid()) {
        _server.send(400, "text/html; charset=utf-8",
                    renderPage("SSID 为空或超长（SSID ≤ 32 字节，密码 ≤ 63 字节）。",
                               true, hostArg, portArg));
        return;
    }
    // host/port 必须成对：只填一个会让设备连到半个地址上
    if (!c.hostPortValid()) {
        _server.send(400, "text/html; charset=utf-8",
                    renderPage("PC 地址与端口必须成对填写（或都留空用编译期默认）。",
                               true, hostArg, portArg));
        return;
    }
    if (!CredentialStore::save(c)) {
        _server.send(500, "text/html; charset=utf-8",
                    renderPage("写入 NVS 失败，请重试。", true, hostArg, portArg));
        return;
    }

    Serial.printf("[配网] 网页提交 SSID=%s，PC=%s:%u，已保存\n",
                  c.ssid.c_str(),
                  c.hasHostPort() ? c.host.c_str() : AIRTYPE_HOST,
                  static_cast<unsigned>(c.hasHostPort() ? c.port
                                                        : AIRTYPE_PORT));
    _saved = true;
    _server.send(200, "text/html; charset=utf-8",
                renderPage("已保存，设备正在重启…", false, hostArg, portArg));
}

void Provisioning::loadTarget(String* host, uint16_t* port) const {
    if (host == nullptr || port == nullptr) {
        return;
    }
    // 只在 NVS 里确实配过地址时才回填；没配过留空，让 secrets.h 继续兜底
    // （否则一保存就把编译期默认冻结进 NVS，以后改 secrets.h 也不生效）。
    airmic::WifiCredentials c;
    if (CredentialStore::load(&c) && c.hasHostPort()) {
        *host = c.host.c_str();
        *port = c.port;
    } else {
        *host = "";
        *port = 0;
    }
}

void Provisioning::handleRescan() {
    refreshScan();  // 阻塞约 1~3s，配网页面可接受
    _server.sendHeader("Location", "/");
    _server.send(302, "text/plain", "");
}

void Provisioning::handleNotFound() {
    // 顺便兼容手机的系统配网探测（/generate_204、/connecttest.txt 等）
    if (_server.method() == HTTP_GET) {
        _server.sendHeader("Location", "/");
        _server.send(302, "text/plain", "");
        return;
    }
    _server.send(404, "text/plain; charset=utf-8", "Not Found");
}

void Provisioning::refreshScan() {
    _scanCount = 0;
    const int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < PROVISION_SCAN_MAX; ++i) {
        _scanSsids[i] = WiFi.SSID(i);
        _scanRssi[i]  = WiFi.RSSI(i);
        ++_scanCount;
    }
    WiFi.scanDelete();

    // 按信号从强到弱插入排序（最多 12 项，开销可忽略）
    for (int i = 1; i < static_cast<int>(_scanCount); ++i) {
        const String ssid = _scanSsids[i];
        const int    rssi = _scanRssi[i];
        int j = i - 1;
        while (j >= 0 && _scanRssi[j] < rssi) {
            _scanSsids[j + 1] = _scanSsids[j];
            _scanRssi[j + 1]  = _scanRssi[j];
            --j;
        }
        _scanSsids[j + 1] = ssid;
        _scanRssi[j + 1]  = rssi;
    }
}

String Provisioning::htmlEscape(const String& in) const {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); ++i) {
        switch (in[i]) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += in[i];    break;
        }
    }
    return out;
}

String Provisioning::renderPage(const char* message, bool isError,
                                const String& host, const String& port) const {
    String html;
    html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<title>AirMic 配网</title><style>"
              "*{box-sizing:border-box}"
              "body{margin:0;padding:24px 16px;background:#f4f5f7;color:#1c1e21;"
              "font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}"
              "main{max-width:460px;margin:0 auto}"
              "h1{font-size:20px;margin:0 0 4px}"
              "h2{font-size:15px;margin:24px 0 8px;color:#5f6368}"
              ".tip{font-size:13px;color:#5f6368;margin:0 0 16px;line-height:1.6}"
              "form{background:#fff;border-radius:10px;padding:16px;"
              "box-shadow:0 1px 3px rgba(0,0,0,.08)}"
              "label{display:block;font-size:13px;margin:12px 0 4px;color:#3c4043}"
              "input{width:100%;padding:10px;font-size:15px;border:1px solid #dadce0;"
              "border-radius:6px;font-family:inherit}"
              "input:focus{outline:none;border-color:#1a73e8}"
              "button{width:100%;margin-top:16px;padding:11px;font-size:15px;"
              "border:0;border-radius:6px;background:#1a73e8;color:#fff;"
              "font-family:inherit;cursor:pointer}"
              "button:active{background:#1765cc}"
              ".msg{margin-top:12px;padding:10px;border-radius:6px;font-size:13px;"
              "line-height:1.5}"
              ".ok{background:#e6f4ea;color:#137333}"
              ".err{background:#fce8e6;color:#c5221f}"
              "table{width:100%;border-collapse:collapse;background:#fff;"
              "border-radius:10px;overflow:hidden;"
              "box-shadow:0 1px 3px rgba(0,0,0,.08)}"
              "th,td{text-align:left;padding:9px 12px;font-size:13px;"
              "border-bottom:1px solid #f1f3f4}"
              "th{background:#f8f9fa;color:#5f6368;font-weight:600}"
              "tr:last-child td{border-bottom:0}"
              ".pick{border:0;background:#e8f0fe;color:#1a73e8;border-radius:4px;"
              "padding:4px 8px;font-size:12px;cursor:pointer;font-family:inherit}"
              ".foot{margin-top:14px;font-size:12px;color:#80868b;line-height:1.6}"
              "code{background:#f1f3f4;padding:1px 4px;border-radius:3px}"
              "</style></head><body><main>");
    html += F("<h1>AirMic 配网</h1>");
    html += F("<p class=\"tip\">选择 Wi-Fi 并填写密码，保存后设备自动重启并接入 "
              "AirTypeIME。换了 PC 就连带把「PC 端地址」改成新机器的 IP。"
              "<br>也可以直接用串口发 <code>SSID:密码</code>。</p>");
    if (message != nullptr) {
        html += F("<div class=\"msg ");
        html += isError ? F("err") : F("ok");
        html += F("\">");
        html += message;  // 仅来自本机常量，不含用户输入
        html += F("</div>");
    }

    html += F("<form method=\"post\" action=\"/save\">"
              "<label for=\"ssid\">Wi-Fi 名称（SSID）</label>"
              "<input id=\"ssid\" name=\"ssid\" list=\"nets\" maxlength=\"32\" "
              "autocomplete=\"off\" required placeholder=\"例如 MyHomeWiFi\">"
              "<datalist id=\"nets\">");
    for (uint16_t i = 0; i < _scanCount; ++i) {
        html += F("<option value=\"");
        html += htmlEscape(_scanSsids[i]);
        html += F("\"></option>");
    }
    html += F("</datalist>"
              "<label for=\"pass\">Wi-Fi 密码</label>"
              "<input id=\"pass\" name=\"pass\" type=\"password\" maxlength=\"63\" "
              "autocomplete=\"off\" placeholder=\"开放网络可留空\">"
              "<label for=\"host\">PC 端地址（AirTypeIME 所在机器）</label>"
              "<input id=\"host\" name=\"host\" maxlength=\"64\" "
              "autocomplete=\"off\" placeholder=\"留空 = 用编译期默认 ");
    html += AIRTYPE_HOST;  // 编译期常量，非用户输入
    html += F("\" value=\"");
    html += htmlEscape(host);
    html += F("\">"
              "<label for=\"port\">端口</label>"
              "<input id=\"port\" name=\"port\" maxlength=\"5\" "
              "inputmode=\"numeric\" autocomplete=\"off\" "
              "placeholder=\"留空 = 用默认 ");
    html += portToString(static_cast<uint16_t>(AIRTYPE_PORT));
    html += F("\" value=\"");
    html += htmlEscape(port);
    html += F("\">"
              "<button type=\"submit\">保存并重启</button>"
              "</form>");

    html += F("<h2>附近热点</h2>");
    if (_scanCount == 0) {
        html += F("<p class=\"tip\">没有扫到热点，点击下方「重新扫描」。</p>");
    } else {
        html += F("<table><tr><th>SSID</th><th>信号</th><th></th></tr>");
        for (uint16_t i = 0; i < _scanCount; ++i) {
            html += F("<tr><td>");
            html += htmlEscape(_scanSsids[i]);
            html += F("</td><td>");
            html += String(_scanRssi[i]);
            html += F(" dBm</td><td><button class=\"pick\" data-ssid=\"");
            html += htmlEscape(_scanSsids[i]);
            html += F("\">使用</button></td></tr>");
        }
        html += F("</table>");
    }

    html += F("<p class=\"foot\"><a href=\"/rescan\">重新扫描</a> · "
              "配网热点不加密，保存后立即关闭。</p>"
              "<script>"
              "document.querySelectorAll('.pick').forEach(function(b){"
              "b.addEventListener('click',function(){"
              "document.getElementById('ssid').value=b.getAttribute('data-ssid');"
              "document.getElementById('pass').focus();});});"
              "</script>");
    html += F("</main></body></html>");
    return html;
}
