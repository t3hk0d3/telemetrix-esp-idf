#include <string.h>

#include "tmx_proto.h"
#include "unity.h"

static void expect(const uint8_t *want, size_t want_len, const uint8_t *out, size_t got_len)
{
    TEST_ASSERT_EQUAL_UINT(want_len, got_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, want_len);
}

TEST_CASE("On upstream example values: tmx_rpt_loop produces [2,0,c]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {2, 0, 0x5A};
    expect(want, sizeof(want), out, tmx_rpt_loop(out, 0x5A));
}

TEST_CASE("On upstream example values: tmx_rpt_digital produces [3,2,pin,v]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {3, 2, 17, 1};
    expect(want, sizeof(want), out, tmx_rpt_digital(out, 17, 1));
}

TEST_CASE("On upstream example values: tmx_rpt_analog produces [4,3,pin,v_hi,v_lo]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {4, 3, 5, 0x0F, 0xFF};
    expect(want, sizeof(want), out, tmx_rpt_analog(out, 5, 0x0FFF));
}

TEST_CASE("On upstream example values: tmx_rpt_firmware produces [4,5,3,2,100]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {4, 5, 3, 2, 100};
    expect(want, sizeof(want), out, tmx_rpt_firmware(out));
}

TEST_CASE("On upstream example values: tmx_rpt_servo_unavailable produces [2,6,pin]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {2, 6, 23};
    expect(want, sizeof(want), out, tmx_rpt_servo_unavailable(out, 23));
}

TEST_CASE("On a read of 3 bytes from 0x76 reg 0xD0: tmx_rpt_i2c_read produces [7,9,3,0x76,0xD0,b0,b1,b2]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t data[] = {0x58, 0x01, 0x02};
    static const uint8_t want[] = {7, 9, 3, 0x76, 0xD0, 0x58, 0x01, 0x02};
    expect(want, sizeof(want), out, tmx_rpt_i2c_read(out, 0x76, 0xD0, data, 3));
}

TEST_CASE("On a read of 0 bytes: tmx_rpt_i2c_read produces a header only", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {4, 9, 0, 0x76, 0xD0};
    expect(want, sizeof(want), out, tmx_rpt_i2c_read(out, 0x76, 0xD0, NULL, 0));
}

TEST_CASE("On a read of 200 bytes: tmx_rpt_i2c_read clamps to 59 and fits TMX_REPORT_MAX", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX + 8];
    uint8_t data[200];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    memset(out, 0xEE, sizeof(out));
    size_t n = tmx_rpt_i2c_read(out, 0x76, 0xD0, data, 200);
    TEST_ASSERT_EQUAL_UINT(TMX_REPORT_MAX, n);
    TEST_ASSERT_EQUAL_UINT8(63, out[0]);
    TEST_ASSERT_EQUAL_UINT8(9, out[1]);
    TEST_ASSERT_EQUAL_UINT8(59, out[2]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out + 5, 59);
    TEST_ASSERT_EQUAL_UINT8(0xEE, out[TMX_REPORT_MAX]);
}

TEST_CASE("On upstream example values: tmx_rpt_i2c_short produces [3,8,1,addr]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {3, 8, 1, 0x76};
    expect(want, sizeof(want), out, tmx_rpt_i2c_short(out, 0x76));
}

TEST_CASE("On upstream example values: tmx_rpt_sonar produces [4,10,trig,cm_hi,cm_lo]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t want[] = {4, 10, 12, 0x01, 0x2C};
    expect(want, sizeof(want), out, tmx_rpt_sonar(out, 12, 300));
}

TEST_CASE("On humidity 65.2 and temperature 35.1: tmx_rpt_dht_data produces little-endian floats", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    /* 65.2f = 0x42826666, 35.1f = 0x420C6666 */
    static const uint8_t want[] = {11, 11, 0, 4, 0x66, 0x66, 0x82, 0x42, 0x66, 0x66, 0x0C, 0x42};
    expect(want, sizeof(want), out, tmx_rpt_dht_data(out, 4, 65.2f, 35.1f));
}

TEST_CASE("On a checksum error and a timeout: tmx_rpt_dht_error produces [4,11,1,pin,err]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t checksum[] = {4, 11, 1, 4, 1};
    static const uint8_t timeout[] = {4, 11, 1, 4, 2};
    expect(checksum, sizeof(checksum), out, tmx_rpt_dht_error(out, 4, 1));
    expect(timeout, sizeof(timeout), out, tmx_rpt_dht_error(out, 4, 2));
}

TEST_CASE("On reset: tmx_rpt_onewire produces [3,14,28,presence]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t data[] = {1};
    static const uint8_t want[] = {3, 14, 28, 1};
    expect(want, sizeof(want), out, tmx_rpt_onewire(out, TMX_CMD_ONE_WIRE_RESET, data, 1));
}

TEST_CASE("On read: tmx_rpt_onewire produces [3,14,32,byte]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t data[] = {0xA5};
    static const uint8_t want[] = {3, 14, 32, 0xA5};
    expect(want, sizeof(want), out, tmx_rpt_onewire(out, TMX_CMD_ONE_WIRE_READ, data, 1));
}

TEST_CASE("On crc8: tmx_rpt_onewire produces [3,14,35,crc]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t data[] = {0x77};
    static const uint8_t want[] = {3, 14, 35, 0x77};
    expect(want, sizeof(want), out, tmx_rpt_onewire(out, TMX_CMD_ONE_WIRE_CRC8, data, 1));
}

TEST_CASE("On search: tmx_rpt_onewire produces [10,14,34,8 bytes]", "[reports]")
{
    uint8_t out[TMX_REPORT_MAX];
    static const uint8_t data[] = {0x28, 1, 2, 3, 4, 5, 6, 0x99};
    static const uint8_t want[] = {10, 14, 34, 0x28, 1, 2, 3, 4, 5, 6, 0x99};
    expect(want, sizeof(want), out, tmx_rpt_onewire(out, TMX_CMD_ONE_WIRE_SEARCH, data, 8));
}
