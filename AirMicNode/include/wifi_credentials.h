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
// 注：AirTypeIME 的 host/port 目前仍是编译期常量（secrets.h /
// airmic_config.h），不在凭据结构里。以后若要网页配 host，
// 给结构加字段并同步两个 parse/format 函数即可，旧格式保持兼容。

#include <cstddef>
#include <string>

namespace airmic {

// 802.11 限制：SSID 最长 32 字节，WPA 口令最长 63 字节。
// 加 AIRMIC_ 前缀：ESP-IDF 的 esp_wifi_types.h 已经占了 MAX_SSID_LEN。
constexpr size_t AIRMIC_SSID_MAX_LEN = 32;
constexpr size_t AIRMIC_PASS_MAX_LEN = 63;

struct WifiCredentials {
    std::string ssid;
    std::string pass;

    /// 写入 NVS 前的最终校验：SSID 非空且长度不超 802.11 上限。
    bool valid() const {
        return !ssid.empty() &&
               ssid.size() <= AIRMIC_SSID_MAX_LEN &&
               pass.size() <= AIRMIC_PASS_MAX_LEN;
    }
};

/// 解析 "SSID:PASS"。两侧空白会被去掉；SSID 为空或超长返回 false。
bool parseSimpleCredentials(const std::string& line, WifiCredentials* out);

/// 解析 {"ssid":"...","pass":"..."}。SSID 缺失/为空/超长返回 false；
/// pass 键存在但值不是合法字符串同样返回 false（不静默当成空密码）。
bool parseJsonCredentials(const std::string& text, WifiCredentials* out);

/// 输出 "SSID:PASS"；密码为空时输出 "SSID:"，保证往返无歧义。
std::string formatSimpleCredentials(const WifiCredentials& c);

/// 输出紧凑 JSON（非 ASCII 字节按 UTF-8 原样输出）。
std::string formatJsonCredentials(const WifiCredentials& c);

/// application/x-www-form-urlencoded 解码：%XX -> 字节，'+' -> 空格。
/// 只用于网页 POST body，不要用在串口原始输入上。
/// 非法转义（% 后不足两位十六进制）按字面量保留，不报错。
std::string urlDecode(const std::string& in);

}  // namespace airmic
