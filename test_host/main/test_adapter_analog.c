#include "adapter_fixture.h"

TEST_CASE("SET_PIN_MODE on analog on a pin with an ADC channel: claimed as analog, the first sample is reported",
          "[adapter]")
{
    fx_start();
    tmx_fake_set_adc(1, 6, 0x0123);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 4, 1);
    TEST_ASSERT_EQUAL(TMX_USE_ANALOG, tmx_pin_use(&fx_a.claims, 34));
    TEST_ASSERT_EQUAL_UINT32(19, tmx_adapter_poll(&fx_a, 0));
    tmx_adapter_poll(&fx_a, 19);
    const uint8_t want[] = {4, 3, 34, 0x01, 0x23};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 5);
    TEST_ASSERT_FALSE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(1, fx_reports[0].kind);
    TEST_ASSERT_EQUAL(34, fx_reports[0].key);
}

TEST_CASE("Analog reporting: a report every interval only when |delta| >= differential", "[adapter]")
{
    fx_start();
    tmx_fake_set_adc(1, 6, 100);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 10, 1);
    tmx_adapter_poll(&fx_a, 19);                /* first sample */
    fx_n_reports = 0;
    tmx_fake_set_adc(1, 6, 109);                /* delta 9 */
    tmx_adapter_poll(&fx_a, 38);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    tmx_fake_set_adc(1, 6, 110);                /* delta 10 from the last reported value */
    tmx_adapter_poll(&fx_a, 57);
    const uint8_t want[] = {4, 3, 34, 0, 110};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 5);
    tmx_fake_set_adc(1, 6, 100);                /* downward delta counts too */
    tmx_adapter_poll(&fx_a, 76);
    TEST_ASSERT_EQUAL(2, fx_n_reports);
}

TEST_CASE("Analog reporting: nothing happens before the interval has elapsed", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    tmx_adapter_poll(&fx_a, 18);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_UINT32(1, tmx_adapter_poll(&fx_a, 18));
}

TEST_CASE("SET_PIN_MODE on analog on a pin without an ADC channel: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 3, 0, 0, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_ANALOG_SCANNING_INTERVAL: the new interval applies; one below the floor is raised to it", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND(TMX_CMD_SET_ANALOG_SCANNING_INTERVAL, 0, 5);
    tmx_adapter_poll(&fx_a, 19);                /* armed with the old interval */
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL_UINT32(10, tmx_adapter_poll(&fx_a, 19));
    tmx_adapter_poll(&fx_a, 29);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    FX_SEND(TMX_CMD_SET_ANALOG_SCANNING_INTERVAL, 29, 50);
    tmx_adapter_poll(&fx_a, 39);                /* due under the previous interval */
    TEST_ASSERT_EQUAL_UINT32(50, tmx_adapter_poll(&fx_a, 39));
}

TEST_CASE("MODIFY_REPORTING analog disable and enable: reports stop and resume", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    tmx_adapter_poll(&fx_a, 19);
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 19, 3, 34);
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 100));
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 100, 1, 34);
    tmx_adapter_poll(&fx_a, 119);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("STOP_ALL_REPORTS: no analog report until ENABLE_ALL_REPORTS", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND0(TMX_CMD_STOP_ALL_REPORTS, 0);
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    FX_SEND0(TMX_CMD_ENABLE_ALL_REPORTS, 100);
    tmx_adapter_poll(&fx_a, 119);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("Analog and digital reporting together: poll returns the nearest deadline", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    TEST_ASSERT_EQUAL_UINT32(2, tmx_adapter_poll(&fx_a, 0));
    TEST_ASSERT_EQUAL_UINT32(2, tmx_adapter_poll(&fx_a, 2));
}

TEST_CASE("SET_PIN_MODE from analog to output: the analog report stops", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 1, 34, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());         /* pin 34 has no output capability: the analog mode stays */
    TEST_ASSERT_EQUAL(TMX_USE_ANALOG, tmx_pin_use(&fx_a.claims, 34));
    FX_SEND(TMX_CMD_SET_PIN_MODE, 1, 34, 0, 0);
    TEST_ASSERT_EQUAL(TMX_USE_INPUT, tmx_pin_use(&fx_a.claims, 34));
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}
