#pragma once
// NVS 凭据存储：基于 ESP32 核心的 Preferences（ESP-IDF NVS 的键值封装）。
//
// 只存 Wi-Fi 凭据；AIRTYPE_HOST/PORT 仍是编译期常量（secrets.h）。
// 存储格式就是 airmic::formatJsonCredentials 产出的 JSON，
// 因此以后要加 host/port 字段时，旧数据依然能解析（未知字段被忽略）。
//
// 用法：
//   airmic::WifiCredentials c;
//   if (CredentialStore::load(&c)) { ... }   // 读
//   CredentialStore::save(c);                 // 写
//   CredentialStore::clear();                 // 恢复「开机就要配网」

#include "wifi_credentials.h"

class CredentialStore {
public:
    static constexpr const char* NAMESPACE  = "airmic";
    static constexpr const char* KEY_CREDS  = "creds";

    /// 读凭据。NVS 里没有、格式损坏或校验失败时返回 false。
    static bool load(airmic::WifiCredentials* out);

    /// 写凭据。c.valid() 为假或写入失败返回 false。
    static bool save(const airmic::WifiCredentials& c);

    /// 删除已存凭据。
    static bool clear();

    /// NVS 里是否已有可用凭据。
    static bool exists();
};
