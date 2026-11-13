#include "credential_store.h"

#include <Preferences.h>

bool CredentialStore::load(airmic::WifiCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    Preferences p;
    if (!p.begin(NAMESPACE, true)) {  // 只读打开
        return false;
    }
    const String json = p.getString(KEY_CREDS, "");
    p.end();

    if (json.isEmpty()) {
        return false;
    }
    return airmic::parseJsonCredentials(json.c_str(), out);
}

bool CredentialStore::save(const airmic::WifiCredentials& c) {
    if (!c.valid()) {
        return false;
    }
    Preferences p;
    if (!p.begin(NAMESPACE, false)) {  // 可写
        return false;
    }
    p.clear();  // 擦掉旧键，避免残留字段
    const std::string json = airmic::formatJsonCredentials(c);
    const size_t written = p.putString(KEY_CREDS, json.c_str());
    p.end();
    return written == json.size();
}

bool CredentialStore::clear() {
    Preferences p;
    if (!p.begin(NAMESPACE, false)) {
        return false;
    }
    const bool ok = p.clear();
    p.end();
    return ok;
}

bool CredentialStore::exists() {
    airmic::WifiCredentials c;
    return load(&c);
}
