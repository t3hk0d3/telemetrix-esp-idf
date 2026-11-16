#include <string.h>

#include "tmx_proto.h"
#include "unity.h"

TEST_CASE("On the handshake versions: tmx_rpt_firmware_long produces [7,5,3,2,100,0xA7,1,0]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t want[] = {7, 5, 3, 2, 100, 0xA7, 1, 0};
    size_t n = tmx_rpt_firmware_long(out, TMX_EXT_MAJOR, TMX_EXT_MINOR);
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On no slot and no code: tmx_rpt_error produces [5,57,seq_hi,seq_lo,cmd,reason]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t want[] = {5, 57, 0x01, 0x02, 2, 1};
    size_t n = tmx_rpt_error(out, 0x0102, 2, 1, -1, 0, -1);
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On a slot and sub: tmx_rpt_error appends [slot,sub]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t want[] = {7, 57, 0, 9, 60, 12, 3, 4};
    size_t n = tmx_rpt_error(out, 9, 60, 12, 3, 4, -1);
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On a slot, sub and plugin code: tmx_rpt_error appends [slot,sub,code]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t want[] = {8, 57, 0xFF, 0xFF, 60, 10, 1, 2, 7};
    size_t n = tmx_rpt_error(out, 0xFFFF, 60, 10, 1, 2, 7);
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On a tag and a seq: tmx_rpt_sync_reply produces [5,58,tag_hi,tag_lo,seq_hi,seq_lo]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t want[] = {5, 58, 0xAB, 0xCD, 0x00, 0x2A};
    size_t n = tmx_rpt_sync_reply(out, 0xABCD, 42);
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On a stock I2C read report on bus 2: tmx_rpt_bus_wrap produces [len,59,2,9,count,addr,reg,data...]",
          "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t stock[] = {7, 9, 2, 0x48, 0x01, 0xAA, 0xBB, 0xCC};
    const uint8_t want[] = {9, 59, 2, 9, 2, 0x48, 0x01, 0xAA, 0xBB, 0xCC};
    size_t n = tmx_rpt_bus_wrap(out, 2, stock, sizeof(stock));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On a stock report whose length byte disagrees: tmx_rpt_bus_wrap returns 0", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t stock[] = {9, 9, 2};
    TEST_ASSERT_EQUAL_size_t(0, tmx_rpt_bus_wrap(out, 1, stock, sizeof(stock)));
}

TEST_CASE("On a slot, sub and data: tmx_rpt_plugin produces [len,61,slot,sub,data...]", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const uint8_t data[] = {0xDE, 0xAD};
    const uint8_t want[] = {5, 61, 3, 1, 0xDE, 0xAD};
    size_t n = tmx_rpt_plugin(out, 3, 1, data, sizeof(data));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, sizeof(want));
}

TEST_CASE("On 251 data bytes: tmx_rpt_plugin fills a 255-byte report", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    uint8_t data[251];
    memset(data, 0x5A, sizeof(data));
    TEST_ASSERT_EQUAL_size_t(255, tmx_rpt_plugin(out, 0, 0, data, sizeof(data)));
    TEST_ASSERT_EQUAL_UINT8(254, out[0]);
}

TEST_CASE("On 252 data bytes: tmx_rpt_plugin returns 0", "[ext_reports]")
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    uint8_t data[252] = {0};
    TEST_ASSERT_EQUAL_size_t(0, tmx_rpt_plugin(out, 0, 0, data, sizeof(data)));
}
