#include "wifi_credentials.h"

#include <cstddef>
#include <cstdio>

namespace airmic {
namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && isSpace(s[b])) {
        ++b;
    }
    while (e > b && isSpace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void appendUtf8(std::string* out, unsigned cp) {
    if (cp <= 0x7F) {
        out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// 读 4 位十六进制，`*i` 指向 'u'，成功后指向最后一位十六进制数字。
bool readHex4(const std::string& s, size_t* i, unsigned* out) {
    if (*i + 4 >= s.size()) {
        return false;
    }
    unsigned v = 0;
    for (int k = 1; k <= 4; ++k) {
        const int d = hexVal(s[*i + k]);
        if (d < 0) {
            return false;
        }
        v = (v << 4) | static_cast<unsigned>(d);
    }
    *i += 4;
    *out = v;
    return true;
}

/// 解析 start 处的 JSON 字符串（s[start] 必须是 '"'），成功时返回 true。
/// 支持 \" \\ \/ \b \f \n \r \t 与 \uXXXX（含代理对）。
bool parseJsonString(const std::string& s, size_t start, std::string* out) {
    if (start >= s.size() || s[start] != '"') {
        return false;
    }
    std::string v;
    for (size_t i = start + 1; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            *out = v;
            return true;
        }
        if (c != '\\') {
            v.push_back(c);
            continue;
        }
        ++i;
        if (i >= s.size()) {
            return false;
        }
        switch (s[i]) {
            case '"':  v.push_back('"');  break;
            case '\\': v.push_back('\\'); break;
            case '/':  v.push_back('/');  break;
            case 'b':  v.push_back('\b'); break;
            case 'f':  v.push_back('\f'); break;
            case 'n':  v.push_back('\n'); break;
            case 'r':  v.push_back('\r'); break;
            case 't':  v.push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!readHex4(s, &i, &cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // 高代理：必须紧跟一个低代理
                    if (i + 2 >= s.size() || s[i + 1] != '\\' || s[i + 2] != 'u') {
                        return false;
                    }
                    const size_t mark = i;
                    i += 2;
                    unsigned lo = 0;
                    if (!readHex4(s, &i, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        i = mark;
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;  // 孤立的低代理
                }
                appendUtf8(&v, cp);
                break;
            }
            default:
                return false;  // 非法转义
        }
    }
    return false;  // 字符串未闭合
}

enum JsonExtract {
    JSON_NOT_FOUND = 0,
    JSON_OK        = 1,
    JSON_MALFORMED = -1,
};

/// 在 text 里找 "key" 对应的字符串值。
/// 找不到返回 JSON_NOT_FOUND；结构对但值不是合法字符串返回 JSON_MALFORMED。
/// 键出现在别的字符串值里（误命中）时会继续往后找。
int extractJsonString(const std::string& text, const char* key,
                      std::string* out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t from = 0;
    while (true) {
        const size_t p = text.find(needle, from);
        if (p == std::string::npos) {
            return JSON_NOT_FOUND;
        }
        size_t q = p + needle.size();
        while (q < text.size() && isSpace(text[q])) {
            ++q;
        }
        if (q >= text.size() || text[q] != ':') {
            from = p + needle.size();  // 误命中，继续找
            continue;
        }
        ++q;
        while (q < text.size() && isSpace(text[q])) {
            ++q;
        }
        if (q >= text.size() || text[q] != '"') {
            return JSON_MALFORMED;
        }
        std::string value;
        if (!parseJsonString(text, q, &value)) {
            return JSON_MALFORMED;
        }
        *out = value;
        return JSON_OK;
    }
}

void escapeJsonString(const std::string& in, std::string* out) {
    out->push_back('"');
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        switch (c) {
            case '"':  *out += "\\\""; break;
            case '\\': *out += "\\\\"; break;
            case '\b': *out += "\\b";  break;
            case '\f': *out += "\\f";  break;
            case '\n': *out += "\\n";  break;
            case '\r': *out += "\\r";  break;
            case '\t': *out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 其余控制字符必须转义，否则 JSON 不合法
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    *out += buf;
                } else {
                    // 非 ASCII 字节按原样输出（调用方保证是 UTF-8）
                    out->push_back(c);
                }
        }
    }
    out->push_back('"');
}

}  // namespace

bool parseSimpleCredentials(const std::string& line, WifiCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    const std::string s = trim(line);
    if (s.empty()) {
        return false;
    }

    std::string ssid;
    std::string pass;
    const size_t colon = s.find(':');
    if (colon == std::string::npos) {
        ssid = s;  // 省略冒号 = 开放网络
    } else {
        // 只在第一个冒号切分，密码里允许出现冒号
        ssid = trim(s.substr(0, colon));
        pass = trim(s.substr(colon + 1));
    }

    WifiCredentials c;
    c.ssid = ssid;
    c.pass = pass;
    if (!c.valid()) {
        return false;
    }
    *out = c;
    return true;
}

bool parseJsonCredentials(const std::string& text, WifiCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    std::string ssid;
    if (extractJsonString(text, "ssid", &ssid) != JSON_OK) {
        return false;
    }

    std::string pass;
    const int passResult = extractJsonString(text, "pass", &pass);
    if (passResult == JSON_MALFORMED) {
        return false;
    }
    if (passResult == JSON_NOT_FOUND) {
        // password 是 pass 的别名；两者都缺省 = 开放网络
        if (extractJsonString(text, "password", &pass) == JSON_MALFORMED) {
            return false;
        }
    }

    WifiCredentials c;
    c.ssid = ssid;
    c.pass = pass;
    if (!c.valid()) {
        return false;
    }
    *out = c;
    return true;
}

std::string formatSimpleCredentials(const WifiCredentials& c) {
    return c.ssid + ":" + c.pass;
}

std::string formatJsonCredentials(const WifiCredentials& c) {
    std::string json = "{\"ssid\":";
    escapeJsonString(c.ssid, &json);
    json += ",\"pass\":";
    escapeJsonString(c.pass, &json);
    json += "}";
    return json;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c == '%' && i + 2 < in.size()) {
            const int hi = hexVal(in[i + 1]);
            const int lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(c);  // 非法转义按字面量保留
    }
    return out;
}

}  // namespace airmic
