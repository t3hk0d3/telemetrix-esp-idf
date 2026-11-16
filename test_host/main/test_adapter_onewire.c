#include "adapter_fixture.h"

static const telemetrix_onewire_bus_t fixed_ow = {.bus = 0, .gpio = 23};
static const telemetrix_onewire_bus_t choose_ow = {.bus = 0, .gpio = -1};

static void start_open(void)
{
    fx_start_buses(NULL, &fixed_ow);
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 4);
    TEST_ASSERT_EQUAL(0, fx_refused());
    tmx_fake_reset();
}

TEST_CASE("ONE_WIRE commands before INIT: refused_commands +1 each, no driver call", "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET, 0);
    FX_SEND(TMX_CMD_ONE_WIRE_SELECT, 0, 1, 2, 3, 4, 5, 6, 7, 8);
    FX_SEND0(TMX_CMD_ONE_WIRE_SKIP, 0);
    FX_SEND(TMX_CMD_ONE_WIRE_WRITE, 0, 0x44, 1);
    FX_SEND0(TMX_CMD_ONE_WIRE_READ, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET_SEARCH, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    TEST_ASSERT_EQUAL(7, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("ONE_WIRE_INIT on a bus 0 with a fixed gpio: opened on that gpio, payload ignored, claimed with one RMT RX",
          "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 4);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("ow_open(0,23)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_ONE_WIRE, tmx_pin_use(&fx_a.claims, 23));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 4));
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);
}

TEST_CASE("ONE_WIRE_INIT on a client-chooses bus 0: opened on the payload pin, claimed", "[adapter]")
{
    fx_start_buses(NULL, &choose_ow);
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 23);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("ow_open(0,23)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_ONE_WIRE, tmx_pin_use(&fx_a.claims, 23));
}

TEST_CASE("ONE_WIRE_INIT on a client-chooses bus 0 with a pin lacking the capability: refused, nothing claimed",
          "[adapter]")
{
    fx_start_buses(NULL, &choose_ow);
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 12);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("ONE_WIRE_INIT with no RMT RX channel left: refused, the pin is not claimed, no open", "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&fx_a.claims));
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 4);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ow_open"));
}

TEST_CASE("ONE_WIRE_INIT when the driver cannot open: refused, pin and RMT RX released", "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    tmx_fake_fail_next("ow_open", ESP_FAIL);
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 4);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
}

TEST_CASE("ONE_WIRE_INIT with no 1-wire bus 0 in the profile: refused_commands +1", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 23);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("ONE_WIRE_INIT while open: no-op success", "[adapter]")
{
    start_open();
    FX_SEND(TMX_CMD_ONE_WIRE_INIT, 0, 4);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ow_open"));
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);
}

TEST_CASE("ONE_WIRE_RESET with a device present: [3,14,28,1] as a reply", "[adapter]")
{
    start_open();
    const uint8_t roms[1][8] = {{0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x81}};
    tmx_fake_ow_devices(0, roms, 1);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET, 0);
    const uint8_t want[] = {3, 14, 28, 1};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
}

TEST_CASE("ONE_WIRE_RESET with no device: [3,14,28,0]", "[adapter]")
{
    start_open();
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET, 0);
    const uint8_t want[] = {3, 14, 28, 0};
    fx_assert_report(0, want, sizeof(want));
}

TEST_CASE("ONE_WIRE_SELECT [8 bytes]: the match ROM command 0x55 then the 8 bytes are written, no report", "[adapter]")
{
    start_open();
    FX_SEND(TMX_CMD_ONE_WIRE_SELECT, 0, 1, 2, 3, 4, 5, 6, 7, 8);
    TEST_ASSERT_EQUAL_STRING("ow_write(0,85,0)\now_write(0,1,0)\now_write(0,2,0)\now_write(0,3,0)\now_write(0,4,0)\n"
                             "ow_write(0,5,0)\now_write(0,6,0)\now_write(0,7,0)\now_write(0,8,0)\n",
                             tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("ONE_WIRE_SKIP: the skip ROM command 0xCC is written", "[adapter]")
{
    start_open();
    FX_SEND0(TMX_CMD_ONE_WIRE_SKIP, 0);
    TEST_ASSERT_EQUAL_STRING("ow_write(0,204,0)\n", tmx_fake_calls());
}

TEST_CASE("ONE_WIRE_WRITE [byte,power]: the byte is written with parasitic power as asked", "[adapter]")
{
    start_open();
    FX_SEND(TMX_CMD_ONE_WIRE_WRITE, 0, 0x44, 1);
    FX_SEND(TMX_CMD_ONE_WIRE_WRITE, 0, 0xBE, 0);
    TEST_ASSERT_EQUAL_STRING("ow_write(0,68,1)\now_write(0,190,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("ONE_WIRE_READ: [3,14,32,byte] as a reply", "[adapter]")
{
    start_open();
    FX_SEND0(TMX_CMD_ONE_WIRE_READ, 0);
    const uint8_t want[] = {3, 14, 32, 0xff};
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
}

TEST_CASE("ONE_WIRE_SEARCH with a device: [10,14,34,rom]; with no more devices: [10,14,34,ff x8]", "[adapter]")
{
    start_open();
    const uint8_t roms[1][8] = {{0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x81}};
    tmx_fake_ow_devices(0, roms, 1);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    const uint8_t first[] = {10, 14, 34, 0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x81};
    const uint8_t none[] = {10, 14, 34, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(0, first, sizeof(first));
    fx_assert_report(1, none, sizeof(none));
    TEST_ASSERT_TRUE(fx_reports[1].reply);
}

TEST_CASE("ONE_WIRE_RESET_SEARCH: the driver search restarts, the same ROM is found again", "[adapter]")
{
    start_open();
    const uint8_t roms[1][8] = {{1, 2, 3, 4, 5, 6, 7, 8}};
    tmx_fake_ow_devices(0, roms, 1);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET_SEARCH, 0);
    FX_SEND0(TMX_CMD_ONE_WIRE_SEARCH, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("ow_reset_search"));
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fx_reports[0].data, fx_reports[1].data, 11);
}

TEST_CASE("ONE_WIRE_CRC8 on the stock client's 28 FF 4C 61 92 16 05 plus its extra byte: [3,14,35,crc of the 7 bytes]",
          "[adapter]")
{
    start_open();
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 7, 0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x81);
    const uint8_t want[] = {3, 14, 35, 0x81};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
}

TEST_CASE("ONE_WIRE_CRC8 of n = 0: [3,14,35,0]", "[adapter]")
{
    start_open();
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 0, 0x55);
    const uint8_t want[] = {3, 14, 35, 0};
    fx_assert_report(0, want, sizeof(want));
}

TEST_CASE("ONE_WIRE_CRC8 does not need an open bus", "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    FX_SEND(TMX_CMD_ONE_WIRE_CRC8, 0, 1, 0x02, 0);
    const uint8_t want[] = {3, 14, 35, 0xbc};
    TEST_ASSERT_EQUAL(0, fx_refused());
    fx_assert_report(0, want, sizeof(want));
}

TEST_CASE("RESET with the 1-wire bus open: ow_close(0), pin and RMT RX released, commands refused afterwards",
          "[adapter]")
{
    start_open();
    FX_SEND0(TMX_CMD_RESET, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("ow_close"));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    FX_SEND0(TMX_CMD_ONE_WIRE_RESET, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("Release with the 1-wire bus never opened: no ow_close", "[adapter]")
{
    fx_start_buses(NULL, &fixed_ow);
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ow_close"));
}

TEST_CASE("ONE_WIRE_INIT with no RMT TX channel left: EXHAUSTED, the RX channel and the pin released, no open",
          "[adapter]")
{
    static const uint8_t p[] = {4};
    fx_start_buses(NULL, &fixed_ow);
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_tx(&fx_a.claims));     /* the grant has one TX channel */
    TEST_ASSERT_EQUAL(TELEMETRIX_REASON_EXHAUSTED, tmx_adapter_command(&fx_a, TMX_CMD_ONE_WIRE_INIT, p, 1, 0));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("ow_open"));
}
