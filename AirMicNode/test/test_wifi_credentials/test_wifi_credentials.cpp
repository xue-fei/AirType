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

void test_json_host_port_basic() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"pass\":\"p\",\"host\":\"192.168.2.177\",\"port\":8765}",
        &c));
    TEST_ASSERT_EQUAL_STRING("192.168.2.177", c.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(8765, c.port);
    TEST_ASSERT_TRUE(c.hasHostPort());
    TEST_ASSERT_TRUE(c.hostPortValid());
}

void test_json_port_as_string() {
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"pc.local\",\"port\":\"9000\"}", &c));
    TEST_ASSERT_EQUAL_STRING("pc.local", c.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(9000, c.port);
}

void test_json_host_port_must_come_in_pair() {
    WifiCredentials c;
    // 只填 host 或只填 port 都是半成品地址，直接拒
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"192.168.2.177\"}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"port\":8765}", &c));
}

void test_json_host_port_range_and_charset() {
    WifiCredentials c;
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"h\",\"port\":0}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"h\",\"port\":65536}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"h\",\"port\":-1}", &c));
    // 端口写进 host、空格、URL 元字符都要挡住，否则会拼坏 ws://host:port/
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"192.168.2.177:8080\",\"port\":80}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"bad host\",\"port\":80}", &c));
    TEST_ASSERT_FALSE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"ws://x\",\"port\":80}", &c));
    // 边界值：65535 仍合法
    TEST_ASSERT_TRUE(parseJsonCredentials(
        "{\"ssid\":\"S\",\"host\":\"h\",\"port\":65535}", &c));
}

void test_json_without_host_port_still_parses() {
    // 配网功能上线前存的旧数据必须继续可用（host 空、port 0 = 用编译期默认）
    WifiCredentials c;
    TEST_ASSERT_TRUE(parseJsonCredentials("{\"ssid\":\"S\",\"pass\":\"p\"}", &c));
    TEST_ASSERT_EQUAL_STRING("", c.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(0, c.port);
    TEST_ASSERT_FALSE(c.hasHostPort());
    TEST_ASSERT_TRUE(c.hostPortValid());
}

void test_json_host_port_round_trip() {
    WifiCredentials in;
    in.ssid = "My \"WiFi\"";
    in.pass = "p\\a";
    in.host = "192.168.2.177";
    in.port = 8765;
    const std::string json = formatJsonCredentials(in);

    WifiCredentials out;
    TEST_ASSERT_TRUE(parseJsonCredentials(json, &out));
    TEST_ASSERT_EQUAL_STRING(in.host.c_str(), out.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(in.port, out.port);
}

void test_json_host_port_not_emitted_when_unset() {
    // 没配过就不输出这两个字段，保持与旧数据一致的存储格式
    WifiCredentials in;
    in.ssid = "S";
    in.pass = "p";
    TEST_ASSERT_EQUAL_STRING("{\"ssid\":\"S\",\"pass\":\"p\"}",
                             formatJsonCredentials(in).c_str());
}

void test_port_text_parse() {
    uint16_t p = 999;
    // 空 / 空白 = 未指定（用编译期默认）
    TEST_ASSERT_TRUE(airmic::parsePortText("", &p));
    TEST_ASSERT_EQUAL_UINT16(0, p);
    TEST_ASSERT_TRUE(airmic::parsePortText("   ", &p));
    TEST_ASSERT_EQUAL_UINT16(0, p);

    TEST_ASSERT_TRUE(airmic::parsePortText("8765", &p));
    TEST_ASSERT_EQUAL_UINT16(8765, p);
    TEST_ASSERT_TRUE(airmic::parsePortText(" 8765 ", &p));
    TEST_ASSERT_EQUAL_UINT16(8765, p);
    TEST_ASSERT_TRUE(airmic::parsePortText("1", &p));
    TEST_ASSERT_EQUAL_UINT16(1, p);
    TEST_ASSERT_TRUE(airmic::parsePortText("65535", &p));
    TEST_ASSERT_EQUAL_UINT16(65535, p);

    TEST_ASSERT_FALSE(airmic::parsePortText("0", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("65536", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("-1", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("+80", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("80a", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("80 80", &p));
    TEST_ASSERT_FALSE(airmic::parsePortText("999999", &p));
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
    RUN_TEST(test_json_host_port_basic);
    RUN_TEST(test_json_port_as_string);
    RUN_TEST(test_json_host_port_must_come_in_pair);
    RUN_TEST(test_json_host_port_range_and_charset);
    RUN_TEST(test_json_without_host_port_still_parses);
    RUN_TEST(test_json_host_port_round_trip);
    RUN_TEST(test_json_host_port_not_emitted_when_unset);
    RUN_TEST(test_port_text_parse);
    RUN_TEST(test_url_decode);
    return UNITY_END();
}
