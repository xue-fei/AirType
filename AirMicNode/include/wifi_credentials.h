#pragma once
// Wi-Fi 凭据的解析 / 格式化 / 校验。
//
// 配网流程（SoftAP 网页、串口回退）把用户输入变成 WifiCredentials，
// 再由 provisioning 模块写进 NVS。本模块刻意只用标准 C++，不依赖
// Arduino / ESP-IDF，因此可以在 PC 上用 [env:native] 跑单元测试。
//
// 支持两种文本格式：
//   1) 简易行格式 "SSID:PASS"（串口回退用）
//      - 密码可空："SSID:"；省略冒号表示开放网络："SSID"
//      - 只在第一个冒号处切分，密码里可以有冒号
//      - 不做 URL 解码（串口输入是字面量）
//   2) JSON {"ssid":"...","pass":"..."}（网页 POST 用）
//      - pass 也可写成 password；两者都在时 pass 优先
//      - pass 可缺省（开放网络）
//      - 未知字段忽略，支持标准 JSON 字符串转义
//
// 另外两个可选字段（缺省 = 用 airmic_config.h / secrets.h 里的编译期默认值）：
//   - "host": PC 端 AirTypeIME 的地址（IP 或主机名）
//   - "port": 端口，写成数字或字符串都行
// 二者必须成对出现，只填一个会被 hostPortValid() 判为非法。
//
// 旧数据（只有 ssid/pass）仍然能解析：host 为空、port 为 0 即「用编译期默认」，
// 因此给结构加字段不会让已配网的设备失效。

#include <cstddef>
#include <cstdint>
#include <string>

namespace airmic {

// 802.11 限制：SSID 最长 32 字节，WPA 口令最长 63 字节。
// 加 AIRMIC_ 前缀：ESP-IDF 的 esp_wifi_types.h 已经占了 MAX_SSID_LEN。
constexpr size_t AIRMIC_SSID_MAX_LEN = 32;
constexpr size_t AIRMIC_PASS_MAX_LEN = 63;

// PC 端地址的长度上限。局域网场景只支持 IP 与域名（不支持 IPv6 文字地址），
// 字符集见 isValidHost()。
constexpr size_t AIRMIC_HOST_MAX_LEN = 64;
constexpr uint16_t AIRMIC_PORT_MIN    = 1;
constexpr uint16_t AIRMIC_PORT_MAX    = 65535;

struct WifiCredentials {
    std::string ssid;
    std::string pass;
    /// PC 端 AirTypeIME 地址。空 = 用编译期默认（airmic_config.h / secrets.h）。
    std::string host;
    /// PC 端端口。0 = 用编译期默认。
    uint16_t    port = 0;

    /// 写入 NVS 前的最终校验：SSID 非空且长度不超 802.11 上限。
    bool valid() const {
        return !ssid.empty() &&
               ssid.size() <= AIRMIC_SSID_MAX_LEN &&
               pass.size() <= AIRMIC_PASS_MAX_LEN;
    }

    /// host/port 是否已经显式配过。
    bool hasHostPort() const { return !host.empty() && port != 0; }

    /// host/port 必须成对出现：要么都空（用编译期默认），要么都合法。
    /// 只填一个会让设备连到半个地址上，直接判非法。
    bool hostPortValid() const {
        if (host.empty()) {
            return port == 0;
        }
        if (port < AIRMIC_PORT_MIN || port > AIRMIC_PORT_MAX) {
            return false;
        }
        if (host.size() > AIRMIC_HOST_MAX_LEN) {
            return false;
        }
        for (size_t i = 0; i < host.size(); ++i) {
            if (!isValidHostChar(host[i])) {
                return false;
            }
        }
        return true;
    }

    /// 地址字符集：字母、数字、点、横线、下划线。
    /// 挡住空格、控制字符与 URL 元字符，避免拼进 ws://host:port/ 时出问题。
    static bool isValidHostChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    }
};

/// 解析 "SSID:PASS"。两侧空白会被去掉；SSID 为空或超长返回 false。
/// 刻意不支持 host/port：串口一行塞不下三段，要配地址请用 JSON。
bool parseSimpleCredentials(const std::string& line, WifiCredentials* out);

/// 解析端口文本（配网表单用）：允许前后空白，只接受纯数字。
/// 空串视为「未指定」，返回 true 且 *out = 0（= 用编译期默认端口）。
/// 含非数字字符、超出 1..65535 返回 false。
bool parsePortText(const std::string& text, uint16_t* out);

/// 解析 {"ssid":"...","pass":"..."[,"host":"..."][,"port":...]}。
/// SSID 缺失/为空/超长返回 false；pass 键存在但值不是合法字符串同样返回
/// false（不静默当成空密码）；host/port 只填一个也返回 false。
bool parseJsonCredentials(const std::string& text, WifiCredentials* out);

/// 输出 "SSID:PASS"；密码为空时输出 "SSID:"，保证往返无歧义。
std::string formatSimpleCredentials(const WifiCredentials& c);

/// 输出紧凑 JSON（非 ASCII 字节按 UTF-8 原样输出）。
/// host/port 只在显式配过时输出，未配则完全不出现（保持旧数据格式）。
std::string formatJsonCredentials(const WifiCredentials& c);

/// application/x-www-form-urlencoded 解码：%XX -> 字节，'+' -> 空格。
/// 只用于网页 POST body，不要用在串口原始输入上。
/// 非法转义（% 后不足两位十六进制）按字面量保留，不报错。
std::string urlDecode(const std::string& in);

}  // namespace airmic
