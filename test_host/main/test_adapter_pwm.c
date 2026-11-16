#include <math.h>

#include "adapter_fixture.h"

static void send_pwm(uint8_t pin, uint8_t res, double freq, uint32_t now)
{
    uint8_t p[12] = {pin, TMX_MODE_PWM, 0, res};
    union {
        double d;
        uint64_t u;
    } v = {.d = freq};
    for (int i = 0; i < 8; i++) {
        p[4 + i] = (uint8_t)(v.u >> (8 * i));
    }
    fx_send(TMX_CMD_SET_PIN_MODE, p, sizeof(p), now);
}

static size_t ledc_calls(void)
{
    return tmx_fake_count("ledc_timer") + tmx_fake_count("ledc_attach") + tmx_fake_count("ledc_duty") +
           tmx_fake_count("ledc_stop") + tmx_fake_count("ledc_timer_free");
}

TEST_CASE("SET_PIN_MODE on PWM: a channel and a timer are claimed, the pin attached", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,5000,8)\nledc_attach(0,0,0,12)\n", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on PWM with the same frequency and resolution twice: the timer is shared", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 5000.0, 0);
    send_pwm(13, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("ledc_timer"));
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,5000,8)\nledc_attach(0,0,0,12)\nledc_attach(0,1,0,13)\n",
                             tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on PWM ignores the client's channel byte", "[adapter]")
{
    fx_start();
    const uint8_t p[12] = {12, TMX_MODE_PWM, 7, 8, 0, 0, 0, 0, 0, 0x88, 0xb3, 0x40};    /* 5000.0 LE, channel 7 */
    fx_send(TMX_CMD_SET_PIN_MODE, p, sizeof(p), 0);
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,5000,8)\nledc_attach(0,0,0,12)\n", tmx_fake_calls());
}

TEST_CASE("ANALOG_WRITE on a PWM pin: sets the duty, clamped to 2^resolution - 1", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 5000.0, 0);
    FX_SEND(TMX_CMD_ANALOG_WRITE, 0, 12, 0, 100);
    FX_SEND(TMX_CMD_ANALOG_WRITE, 0, 12, 0x03, 0xe8);     /* 1000 */
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,5000,8)\nledc_attach(0,0,0,12)\nledc_duty(0,0,100)\n"
                             "ledc_duty(0,0,255)\n", tmx_fake_calls());
}

TEST_CASE("ANALOG_WRITE on a pin not claimed for PWM: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_ANALOG_WRITE, 0, 12, 0, 100);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, 1);
    FX_SEND(TMX_CMD_ANALOG_WRITE, 0, 13, 0, 100);
    TEST_ASSERT_EQUAL(2, fx_refused());
    TEST_ASSERT_EQUAL_STRING("gpio_output(13,0)\n", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on PWM with a fifth distinct frequency: refused_commands +1, no channel taken, the pin's "
          "previous mode untouched", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 1000.0, 0);
    send_pwm(13, 8, 2000.0, 0);
    send_pwm(14, 8, 3000.0, 0);
    send_pwm(15, 8, 4000.0, 0);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 16, 1);
    size_t before = ledc_calls();
    send_pwm(16, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(before, ledc_calls());
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 16));
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 16, 1);    /* still an output */
    TEST_ASSERT_EQUAL(1, fx_refused());
    /* the freed-up channel count is intact: a fifth pin on an existing frequency still gets one */
    send_pwm(16, 8, 1000.0, 0);
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 16));
}

TEST_CASE("SET_PIN_MODE on PWM with a resolution outside 1-14 or a bad frequency: refused_commands +1", "[adapter]")
{
    fx_start();
    send_pwm(12, 0, 5000.0, 0);
    send_pwm(12, 15, 5000.0, 0);
    send_pwm(12, 8, 0.0, 0);
    send_pwm(12, 8, -5.0, 0);
    send_pwm(12, 8, NAN, 0);
    TEST_ASSERT_EQUAL(5, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    send_pwm(12, 14, 5000.0, 0);
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("SET_PIN_MODE on PWM on a pin without the PWM capability: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    send_pwm(4, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SET_PIN_MODE on PWM when the attach fails: refused_commands +1, channel and claim released, safe state",
          "[adapter]")
{
    fx_start();
    tmx_fake_fail_next("ledc_attach", ESP_FAIL);
    send_pwm(12, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,5000,8)\nledc_attach(0,0,0,12)\nledc_stop(0,0)\nledc_timer_free(0,0)\n"
                             "gpio_input(12,0,0)\n", tmx_fake_calls());
    send_pwm(13, 8, 5000.0, 0);                  /* the channel is free again */
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 13));
}

TEST_CASE("SET_PIN_MODE on PWM when the timer setup fails: refused_commands +1, the pin's previous mode untouched",
          "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 1);
    tmx_fake_fail_next("ledc_timer", ESP_FAIL);
    send_pwm(12, 8, 5000.0, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("SET_PIN_MODE from PWM to output: the channel is released first, then the pin reconfigured", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 5000.0, 0);
    tmx_fake_reset();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 12, 1);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\nledc_timer_free(0,0)\ngpio_input(12,0,0)\ngpio_output(12,0)\n",
                             tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("ANALOG_OUT_ATTACH on a pin already in PWM: no-op success", "[adapter]")
{
    fx_start();
    send_pwm(12, 8, 5000.0, 0);
    tmx_fake_reset();
    FX_SEND(TMX_CMD_ANALOG_OUT_ATTACH, 0, 12, 3);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("ANALOG_OUT_ATTACH on any other pin: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_ANALOG_OUT_ATTACH, 0, 12, 3);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, 1);
    FX_SEND(TMX_CMD_ANALOG_OUT_ATTACH, 0, 13, 3);
    TEST_ASSERT_EQUAL(2, fx_refused());
    TEST_ASSERT_EQUAL_STRING("gpio_output(13,0)\n", tmx_fake_calls());
}

TEST_CASE("ANALOG_OUT_DETACH: the channel is released and the pin goes to its safe state", "[adapter]")
{
    fx_start();
    send_pwm(2, 8, 5000.0, 0);
    tmx_fake_reset();
    FX_SEND(TMX_CMD_ANALOG_OUT_DETACH, 0, 2);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\nledc_timer_free(0,0)\ngpio_output(2,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 2));
    FX_SEND(TMX_CMD_ANALOG_OUT_DETACH, 0, 2);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("SERVO_ATTACH: 50 Hz and 14 bits on a claimed channel, the pin attached", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x20, 0x09, 0x60);       /* 544 .. 2400 */
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_SERVO, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,50,14)\nledc_attach(0,0,0,12)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("SERVO_WRITE on angle 90 with min 544, max 2400: pulse 1472 us", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x20, 0x09, 0x60);
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 90);
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 0);
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 180);
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 250);       /* clamped to 180 */
    /* duty = pulse * 2^14 / 20000: 1472 -> 1205, 544 -> 445, 2400 -> 1966 */
    TEST_ASSERT_EQUAL_STRING("ledc_timer(0,0,50,14)\nledc_attach(0,0,0,12)\nledc_duty(0,0,1205)\n"
                             "ledc_duty(0,0,445)\nledc_duty(0,0,1966)\nledc_duty(0,0,1966)\n", tmx_fake_calls());
}

TEST_CASE("SERVO_WRITE on a pin that is not a servo: refused_commands +1, no driver call", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SERVO_WRITE, 0, 12, 90);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SERVO_ATTACH on more servos than max_servo: [2,6,pin] for the extra one, never dropped", "[adapter]")
{
    fx_start();
    const uint8_t pins[] = {12, 13, 14, 15, 16};
    for (size_t i = 0; i < sizeof(pins); i++) {
        const uint8_t p[] = {pins[i], 0x02, 0x20, 0x09, 0x60};
        fx_send(TMX_CMD_SERVO_ATTACH, p, sizeof(p), 0);
    }
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    const uint8_t want[] = {2, 6, 16};
    fx_assert_report(0, want, 3);
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(-1, fx_reports[0].kind);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 16));
}

TEST_CASE("SERVO_ATTACH with no free LEDC channel: [2,6,pin] even below max_servo", "[adapter]")
{
    fx_start();
    fx_profile.ledc.channel_mask = 0x03;        /* two channels; the adapter keeps a pointer to the profile */
    send_pwm(12, 8, 1000.0, 0);
    send_pwm(13, 8, 1000.0, 0);
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 14, 0x02, 0x20, 0x09, 0x60);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    const uint8_t want[] = {2, 6, 14};
    fx_assert_report(0, want, 3);
}

TEST_CASE("SERVO_ATTACH on a pin without the PWM capability or outside the profile: refused_commands +1", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 4, 0x02, 0x20, 0x09, 0x60);
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 99, 0x02, 0x20, 0x09, 0x60);
    TEST_ASSERT_EQUAL(2, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
}

TEST_CASE("SERVO_DETACH: the channel is released and a later servo may use the slot", "[adapter]")
{
    fx_start();
    const uint8_t pins[] = {12, 13, 14, 15};
    for (size_t i = 0; i < sizeof(pins); i++) {
        const uint8_t p[] = {pins[i], 0x02, 0x20, 0x09, 0x60};
        fx_send(TMX_CMD_SERVO_ATTACH, p, sizeof(p), 0);
    }
    tmx_fake_reset();
    FX_SEND(TMX_CMD_SERVO_DETACH, 0, 12);
    TEST_ASSERT_EQUAL_STRING("ledc_stop(0,0)\ngpio_input(12,0,0)\n", tmx_fake_calls());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 16, 0x02, 0x20, 0x09, 0x60);
    TEST_ASSERT_EQUAL(TMX_USE_SERVO, tmx_pin_use(&fx_a.claims, 16));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    FX_SEND(TMX_CMD_SERVO_DETACH, 0, 12);       /* not a servo any more */
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("RESET with PWM and servo pins: LEDC channels stopped before the safe states", "[adapter]")
{
    fx_start();
    send_pwm(2, 8, 5000.0, 0);
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x20, 0x09, 0x60);
    tmx_fake_reset();
    FX_SEND0(TMX_CMD_RESET, 0);
    TEST_ASSERT_EQUAL_STRING("release()\nledc_stop(0,0)\nledc_timer_free(0,0)\nledc_stop(0,1)\nledc_timer_free(0,1)\n"
                             "gpio_output(2,0)\ngpio_input(12,0,0)\n", tmx_fake_calls());
    TEST_ASSERT_TRUE(tmx_adapter_reset_requested(&fx_a));
    send_pwm(2, 8, 5000.0, 0);                  /* every channel and timer is free again */
    TEST_ASSERT_EQUAL(TMX_USE_PWM, tmx_pin_use(&fx_a.claims, 2));
}

TEST_CASE("pin_uses: lists claimed pins with their use names", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 2, 1);
    FX_SEND(TMX_CMD_SERVO_ATTACH, 0, 12, 0x02, 0x20, 0x09, 0x60);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 34, 3, 0, 0, 0);
    telemetrix_pin_use_t uses[8];
    TEST_ASSERT_EQUAL(3, tmx_adapter_pin_uses(&fx_a, uses, 8));
    TEST_ASSERT_EQUAL_STRING("output", uses[0].use);
    TEST_ASSERT_EQUAL(12, uses[1].gpio);
    TEST_ASSERT_EQUAL_STRING("servo", uses[1].use);
    TEST_ASSERT_EQUAL_STRING("analog", uses[2].use);
    TEST_ASSERT_EQUAL(2, tmx_adapter_pin_uses(&fx_a, uses, 2));      /* capped */
}
