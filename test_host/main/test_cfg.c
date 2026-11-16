#include <string.h>

#include "unity.h"
#include "telemetrix.h"

TEST_CASE("telemetrix_config_default: every default of the global constraints", "[cfg]")
{
    telemetrix_config_t c;
    memset(&c, 0xa5, sizeof(c));
    telemetrix_config_default(&c);
    TEST_ASSERT_TRUE(c.server_enabled);
    TEST_ASSERT_EQUAL_UINT16(31336, c.port);
    TEST_ASSERT_EQUAL_UINT8(10, c.keepalive_idle_s);
    TEST_ASSERT_EQUAL_UINT8(2, c.keepalive_intvl_s);
    TEST_ASSERT_EQUAL_UINT8(3, c.keepalive_count);
    TEST_ASSERT_EQUAL_UINT16(10000, c.send_stall_ms);
    TEST_ASSERT_EQUAL_UINT16(4096, c.send_queue_bytes);
    TEST_ASSERT_EQUAL_UINT16(50, c.i2c_timeout_ms);
    TEST_ASSERT_EQUAL_UINT8(10, c.analog_interval_floor_ms);
    TEST_ASSERT_EQUAL_UINT8(6, c.max_dht);
    TEST_ASSERT_EQUAL_UINT8(6, c.max_sonar);
    TEST_ASSERT_EQUAL_UINT8(4, c.max_servo);
    TEST_ASSERT_NULL(c.allowed);
    TEST_ASSERT_EQUAL(0, c.n_allowed);
    TEST_ASSERT_EQUAL_UINT8(5, c.task_priority);
    TEST_ASSERT_EQUAL_INT8(-1, c.task_core);
}

TEST_CASE("telemetrix_config_default with NULL: nothing happens", "[cfg]")
{
    telemetrix_config_default(NULL);
}

TEST_CASE("telemetrix_cidr_parse of an IPv4 network: family, address and prefix", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("192.168.10.0/24", &c));
    TEST_ASSERT_EQUAL_UINT8(AF_INET, c.family);
    const uint8_t want[4] = {192, 168, 10, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, c.addr, 4);
    TEST_ASSERT_EQUAL_UINT8(24, c.prefix);
}

TEST_CASE("telemetrix_cidr_parse of an IPv4 host without a prefix: prefix 32", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("10.0.0.7", &c));
    TEST_ASSERT_EQUAL_UINT8(AF_INET, c.family);
    TEST_ASSERT_EQUAL_UINT8(32, c.prefix);
    TEST_ASSERT_EQUAL_UINT8(7, c.addr[3]);
}

TEST_CASE("telemetrix_cidr_parse of prefix 0 and 32: accepted", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("0.0.0.0/0", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.prefix);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("1.2.3.4/32", &c));
    TEST_ASSERT_EQUAL_UINT8(32, c.prefix);
}

TEST_CASE("telemetrix_cidr_parse of an IPv6 network: family, address and prefix", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("fd00:1234::/48", &c));
    TEST_ASSERT_EQUAL_UINT8(AF_INET6, c.family);
    const uint8_t want[16] = {0xfd, 0x00, 0x12, 0x34};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, c.addr, 16);
    TEST_ASSERT_EQUAL_UINT8(48, c.prefix);
}

TEST_CASE("telemetrix_cidr_parse of an IPv6 host without a prefix: prefix 128", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("2001:db8:0:0:0:0:0:1", &c));
    TEST_ASSERT_EQUAL_UINT8(128, c.prefix);
    const uint8_t want[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, c.addr, 16);
}

TEST_CASE("telemetrix_cidr_parse of compressed IPv6 forms: expanded", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("::1", &c));
    TEST_ASSERT_EQUAL_UINT8(1, c.addr[15]);
    TEST_ASSERT_EQUAL_UINT8(0, c.addr[0]);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("::", &c));
    TEST_ASSERT_EQUAL_UINT8(0, c.addr[15]);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("fe80::/10", &c));
    TEST_ASSERT_EQUAL_UINT8(0xfe, c.addr[0]);
    TEST_ASSERT_EQUAL_UINT8(0x80, c.addr[1]);
    TEST_ASSERT_EQUAL_UINT8(10, c.prefix);
}

TEST_CASE("telemetrix_cidr_parse of an IPv4-mapped IPv6 address: the dotted tail is read", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("::ffff:192.0.2.1", &c));
    TEST_ASSERT_EQUAL_UINT8(AF_INET6, c.family);
    const uint8_t want[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, c.addr, 16);
}

TEST_CASE("telemetrix_cidr_parse of bad text: ESP_ERR_INVALID_ARG", "[cfg]")
{
    static const char *const bad[] = {
        "", "/24", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.-4", "1.2.3.4/33", "1.2.3.4/", "1.2.3.4/-1", "1.2.3.4/24x",
        "1.2.3.4 /24", "01.2.3.4x", "a.b.c.d", "1.2.3.4//8", "::1/129", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1::2::3",
        "12345::1", "g::1", ":1", "1:", ":::", "1.2.3.4::1", "::1.2.3", "fd00::/", "fd00::/1x", "hostname",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        telemetrix_cidr_t c;
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, telemetrix_cidr_parse(bad[i], &c), bad[i]);
    }
}

TEST_CASE("telemetrix_cidr_parse of NULL arguments: ESP_ERR_INVALID_ARG", "[cfg]")
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_cidr_parse(NULL, &c));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_cidr_parse("1.2.3.4", NULL));
}
