#include "adapter_fixture.h"

TEST_CASE("On init: the adapter owns every profile pin in its safe state and counters start at 0", "[adapter]")
{
    fx_start();
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_ignored());
    telemetrix_pin_use_t uses[4];
    TEST_ASSERT_EQUAL(0, tmx_adapter_pin_uses(&fx_a, uses, 4));
}

TEST_CASE("SET_PIN_MODE on output on a profile pin: driver gpio output called once, pin claimed as output", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 1);
    TEST_ASSERT_EQUAL_STRING("gpio_output(2,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 2));
    telemetrix_pin_use_t uses[4];
    TEST_ASSERT_EQUAL(1, tmx_adapter_pin_uses(&fx_a, uses, 4));
    TEST_ASSERT_EQUAL(2, uses[0].gpio);
    TEST_ASSERT_EQUAL_STRING("output", uses[0].use);
}

TEST_CASE("SET_PIN_MODE on input pull-up with report 1: a later level change is reported once", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 4, 2, 1);
    TEST_ASSERT_EQUAL_STRING("gpio_input(4,1,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_INPUT_PULLUP, tmx_pin_use(&fx_a.claims, 4));
    tmx_adapter_poll(&fx_a, 2);
    fx_n_reports = 0;                       /* the first sample is checked in its own test */
    tmx_fake_set_level(4, true);
    tmx_adapter_poll(&fx_a, 4);
    tmx_adapter_poll(&fx_a, 6);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    const uint8_t want[] = {3, 2, 4, 1};
    fx_assert_report(0, want, 4);
    TEST_ASSERT_FALSE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(0, fx_reports[0].kind);
    TEST_ASSERT_EQUAL(4, fx_reports[0].key);
}

TEST_CASE("SET_PIN_MODE on input with report 1: the first sample after the mode change is reported, even a 0",
          "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    tmx_adapter_poll(&fx_a, 2);
    const uint8_t want[] = {3, 2, 12, 0};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 4);
}

TEST_CASE("SET_PIN_MODE on input pull-down with report 0: nothing is reported", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 9, 0);
    TEST_ASSERT_EQUAL_STRING("gpio_input(12,0,1)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_INPUT_PULLDOWN, tmx_pin_use(&fx_a.claims, 12));
    tmx_fake_set_level(12, true);
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("SET_PIN_MODE on input pull-down on a pin without pull-down: refused_commands +1, no driver call",
          "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 4, 9, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 4));
}

TEST_CASE("SET_PIN_MODE on a pin outside the profile: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 99, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on a pin another feature holds: refused_commands +1, the claim and the driver untouched",
          "[adapter]")
{
    fx_start();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_pin(&fx_a.claims, 21, TMX_USE_I2C));
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 21, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_I2C, tmx_pin_use(&fx_a.claims, 21));
}

TEST_CASE("SET_PIN_MODE on touch (mode 7): ignored_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 7);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on an unknown mode: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 5);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE with a payload too short: refused_commands +1, nothing else", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 4, 2);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("An ignored command id: ignored_commands +1 and nothing else happens", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_STEPPER_RUN, 0, 1, 2, 3);
    FX_SEND(TMX_CMD_DAC_WRITE, 0, 1, 2);
    FX_SEND(200, 0, 1);
    TEST_ASSERT_EQUAL(3, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE input after output: the claim follows and the driver reconfigures the pin", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 0, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_INPUT, tmx_pin_use(&fx_a.claims, 2));
    TEST_ASSERT_EQUAL_STRING("gpio_output(2,0)\ngpio_input(2,0,0)\n", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE when the driver fails: refused_commands +1, the pin back in its safe state and released",
          "[adapter]")
{
    fx_start();
    tmx_fake_fail_next("gpio_output", ESP_FAIL);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 1);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL_STRING("gpio_output(12,0)\ngpio_input(12,0,0)\n", tmx_fake_calls());
}

TEST_CASE("DIGITAL_WRITE on an output pin: the driver sets the level", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 1);
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 2, 1);
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 2, 0);
    TEST_ASSERT_EQUAL_STRING("gpio_output(2,0)\ngpio_set(2,1)\ngpio_set(2,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("DIGITAL_WRITE on a pin not claimed for that use: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 2, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 4, 0, 0);
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 4, 1);
    TEST_ASSERT_EQUAL(2, fx_refused());
    TEST_ASSERT_EQUAL_STRING("gpio_input(4,0,0)\n", tmx_fake_calls());
}

TEST_CASE("Digital polling: poll returns 2 ms while an input reports and UINT32_MAX when nothing is scheduled",
          "[adapter]")
{
    fx_start();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 0));
    FX_SEND(TMX_CMD_SET_PIN_MODE, 10, 12, 0, 1);
    TEST_ASSERT_EQUAL_UINT32(2, tmx_adapter_poll(&fx_a, 10));
    TEST_ASSERT_EQUAL_UINT32(1, tmx_adapter_poll(&fx_a, 11));
    TEST_ASSERT_EQUAL_UINT32(2, tmx_adapter_poll(&fx_a, 12));
}

TEST_CASE("MODIFY_REPORTING digital disable and enable: reports stop and resume", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    tmx_adapter_poll(&fx_a, 2);
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 2, 4, 12);
    tmx_fake_set_level(12, true);
    tmx_adapter_poll(&fx_a, 10);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 10));
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 10, 2, 12);
    tmx_adapter_poll(&fx_a, 12);
    const uint8_t want[] = {3, 2, 12, 1};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 4);
}

TEST_CASE("MODIFY_REPORTING mode 0: silences every digital and analog pin and ignores the pin byte", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 0, 12);
    tmx_adapter_poll(&fx_a, 50);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 50));
    TEST_ASSERT_EQUAL(0, fx_refused());
}

TEST_CASE("MODIFY_REPORTING mode 2 after mode 0: re-enables that digital pin only", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, 0, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 1);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 0, 0);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 2, 13);
    tmx_adapter_poll(&fx_a, 2);
    tmx_adapter_poll(&fx_a, 19);
    const uint8_t want[] = {3, 2, 13, 0};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 4);
}

TEST_CASE("ENABLE_ALL_REPORTS after MODIFY_REPORTING mode 0: does not re-enable pins", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 0, 0);
    FX_SEND0(TMX_CMD_ENABLE_ALL_REPORTS, 0);
    tmx_adapter_poll(&fx_a, 50);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("MODIFY_REPORTING with a mode above 4, an unknown pin or the wrong kind of pin: refused_commands +1",
          "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 5, 12);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 2, 99);
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 1, 12);    /* analog enable on a digital input */
    FX_SEND(TMX_CMD_MODIFY_REPORTING, 0, 2, 2);     /* digital enable on an unclaimed pin */
    TEST_ASSERT_EQUAL(4, fx_refused());
}

TEST_CASE("STOP_ALL_REPORTS: no digital report until ENABLE_ALL_REPORTS", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND0(TMX_CMD_STOP_ALL_REPORTS, 0);
    tmx_fake_set_level(12, true);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 100));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    FX_SEND0(TMX_CMD_ENABLE_ALL_REPORTS, 100);
    tmx_adapter_poll(&fx_a, 102);
    const uint8_t want[] = {3, 2, 12, 1};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 4);
}

TEST_CASE("LOOP: replies [2,0,c], never dropped, no coalescing", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_LOOP_COMMAND, 0, 0x5a);
    const uint8_t want[] = {2, 0, 0x5a};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 3);
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(-1, fx_reports[0].kind);
}

TEST_CASE("GET_FIRMWARE_VERSION: replies [4,5,3,2,100], never dropped", "[adapter]")
{
    fx_start();
    FX_SEND0(TMX_CMD_GET_FIRMWARE_VERSION, 0);
    const uint8_t want[] = {4, 5, 3, 2, 100};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, 5);
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(-1, fx_reports[0].kind);
}

TEST_CASE("RESET: every claimed pin to its safe state, reports cleared, reset requested once", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 1);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND0(TMX_CMD_STOP_ALL_REPORTS, 0);
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_RESET, 5);
    TEST_ASSERT_EQUAL_STRING("release()\ngpio_output(2,0)\ngpio_input(12,0,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 2));
    telemetrix_pin_use_t uses[4];
    TEST_ASSERT_EQUAL(0, tmx_adapter_pin_uses(&fx_a, uses, 4));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 5));
    TEST_ASSERT_TRUE(tmx_adapter_reset_requested(&fx_a));
    TEST_ASSERT_FALSE(tmx_adapter_reset_requested(&fx_a));
}

TEST_CASE("release (disconnect): same release and reporting enabled for the next session", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 0, 1);
    FX_SEND0(TMX_CMD_STOP_ALL_REPORTS, 0);
    tmx_adapter_release(&fx_a);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_FALSE(tmx_adapter_reset_requested(&fx_a));
    FX_SEND(TMX_CMD_SET_PIN_MODE, 20, 12, 0, 1);
    tmx_adapter_poll(&fx_a, 22);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}
