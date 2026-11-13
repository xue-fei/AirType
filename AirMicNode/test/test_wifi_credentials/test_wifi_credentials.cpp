#include <unity.h>

#include <string>

#include "wifi_credentials.h"

using namespace airmic;

void setUp() {}
void tearDown() {}

void test_simple_basic_round_trip() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseSimpleCredentials("MyHomeWiFi:secret123", &c));
    TEST_ASSERT_EQUAL_STRING("MyHomeWiFi", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", c.pass.c_str());
    TEST_ASSERT_TRUE(c.valid());
    TEST_ASSERT_EQUAL_STRING("MyHomeWiFi:secret123",
                             formatSimpleCredentials(c).c_str());
}

void test_simple_password_may_contain_colon() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseSimpleCredentials("SSID:pa:ss:word", &c));
    TEST_ASSERT_EQUAL_STRING("SSID", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("pa:ss:word", c.pass.c_str());
}

void test_simple_open_network() {
    WifiCredentials c;
    // 省略冒号 = 开放网络
    TEST_ASSERT_TRUE(parseSimpleCredentials("OpenWiFi", &c));
    TEST_ASSERT_EQUAL_STRING("OpenWiFi", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", c.pass.c_str());
    // 显式空密码
    TEST_ASSERT_TRUE(parseSimpleCredentials("OpenWiFi:", &c));
    TEST_ASSERT_EQUAL_STRING("", c.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("OpenWiFi:", formatSimpleCredentials(c).c_str());
}

void test_simple_trims_surrounding_whitespace() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseSimpleCredentials("  MyWiFi : pass \r\n", &c));
    TEST_ASSERT_EQUAL_STRING("MyWiFi", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("pass", c.pass.c_str());
}

void test_simple_rejects_bad_input() {
    WifiCredentials c;
    TEST_ASSERT_FALSE(parseSimpleCredentials("", &c));
    TEST_ASSERT_FALSE(parseSimpleCredentials("   ", &c));
    TEST_ASSERT_FALSE(parseSimpleCredentials(":onlypass", &c));
    TEST_ASSERT_FALSE(parseSimpleCredentials("a:short", nullptr));
    TEST_ASSERT_FALSE(parseSimpleCredentials(std::string(33, 'a') + ":p", &c));
    TEST_ASSERT_FALSE(parseSimpleCredentials("ssid:" + std::string(64, 'p'), &c));

    // 边界值：32 字节 SSID + 63 字节密码仍然合法
    WifiCredentials ok;
    TEST_ASSERT_TRUE(parseSimpleCredentials(
        std::string(32, 'a') + ":" + std::string(63, 'p'), &ok));
    TEST_ASSERT_TRUE(ok.valid());
}

void test_simple_is_literal_not_url_decoded() {
    // 串口输入是字面量，'+' 和 %XX 不应被解释
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseSimpleCredentials("My+WiFi:p%40ss", &c));
    TEST_ASSERT_EQUAL_STRING("My+WiFi", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("p%40ss", c.pass.c_str());
}

void test_json_basic() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"MyWiFi\",\"pass\":\"secret\"}", &c));
    TEST_ASSERT_EQUAL_STRING("MyWiFi", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", c.pass.c_str());
}

void test_json_password_alias_and_unknown_fields() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{ \"password\" : \"p1\", \"ssid\" : \"S\", \"extra\": 42 }", &c));
    TEST_ASSERT_EQUAL_STRING("S", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("p1", c.pass.c_str());

    // pass 优先于 password
    WifiCredentials d;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"password\":\"p1\",\"pass\":\"p2\"}", &d));
    TEST_ASSERT_EQUAL_STRING("p2", d.pass.c_str());

    // pass 可缺省 = 开放网络
    WifiCredentials e;
    TEST_ASSERT_TRUE(parseJsonCredentials("{\"ssid\":\"S\"}", &e));
    TEST_ASSERT_EQUAL_STRING("", e.pass.c_str());
}

void test_json_escapes() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"pass\":\"a\\\"b\\\\c\\nd\\te\"}", &c));
    TEST_ASSERT_EQUAL_STRING("S", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\nd\te", c.pass.c_str());
}

void test_json_unicode_escape() {
    // 「密码」的 BMP \u 转义
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"pass\":\"\\u5bc6\\u7801\"}", &c));
    TEST_ASSERT_EQUAL_STRING("\xe5\xaf\x86\xe7\xa0\x81", c.pass.c_str());

    // 代理对 U+1F600
    WifiCredentials d;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"pass\":\"\\ud83d\\ude00\"}", &d));
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", d.pass.c_str());
}

void test_json_rejects_bad_input() {
    WifiCredentials c;
    TEST_ASSERT_FALSE(parseJsonCredentials("", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials("{\"pass\":\"x\"}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials("{\"ssid\":\"\",\"pass\":\"x\"}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials("{\"ssid\":123}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials("{\"ssid\":\"S\",\"pass\":}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"pass\":\"unterminated}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        std::string("{\"ssid\":\"") + std::string(33, 'a') + "\"}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials("{\"ssid\":\"S\"}", nullptr));
}

void test_json_round_trip() {
    WifiCredentials in;
    in.ssid = "My \"WiFi\"";
    in.pass = "p\\a\ts\ns";
    const std::string json = formatJsonCredentials(in);

    WifiCredentials out;
    TEST_ASSERT_TRUE(parseJsonCredentials(json, &out));
    TEST_ASSERT_EQUAL_STRING(in.ssid.c_str(), out.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING(in.pass.c_str(), out.pass.c_str());
}

void test_url_decode() {
    TEST_ASSERT_EQUAL_STRING("hello world", urlDecode("hello+world").c_str());
    TEST_ASSERT_EQUAL_STRING("p@ss:w/rd", urlDecode("p%40ss%3Aw%2Frd").c_str());
    TEST_ASSERT_EQUAL_STRING("密码", urlDecode("%E5%AF%86%E7%A0%81").c_str());
    TEST_ASSERT_EQUAL_STRING("100%", urlDecode("100%25").c_str());
    // 非法转义按字面量保留，不报错、不丢字符
    TEST_ASSERT_EQUAL_STRING("a%zzb", urlDecode("a%zzb").c_str());
    TEST_ASSERT_EQUAL_STRING("a%2", urlDecode("a%2").c_str());
    TEST_ASSERT_EQUAL_STRING("", urlDecode("").c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_simple_basic_round_trip);
    RUN_TEST(test_simple_password_may_contain_colon);
    RUN_TEST(test_simple_open_network);
    RUN_TEST(test_simple_trims_surrounding_whitespace);
    RUN_TEST(test_simple_rejects_bad_input);
    RUN_TEST(test_simple_is_literal_not_url_decoded);
    RUN_TEST(test_json_basic);
    RUN_TEST(test_json_password_alias_and_unknown_fields);
    RUN_TEST(test_json_escapes);
    RUN_TEST(test_json_unicode_escape);
    RUN_TEST(test_json_rejects_bad_input);
    RUN_TEST(test_json_round_trip);
    RUN_TEST(test_url_decode);
    return UNITY_END();
}
