#include "adapter_fixture.h"

#define R_NONE TELEMETRIX_REASON_NONE
#define NO_PIN TELEMETRIX_REASON_NO_SUCH_PIN
#define NO_CAP TELEMETRIX_REASON_NO_CAPABILITY
#define BUSY TELEMETRIX_REASON_PIN_BUSY
#define EXHAUSTED TELEMETRIX_REASON_EXHAUSTED
#define BAD_ARG TELEMETRIX_REASON_BAD_ARGUMENT
#define BUS_UNKNOWN TELEMETRIX_REASON_BUS_UNKNOWN
#define NOT_IN_SCOPE TELEMETRIX_REASON_NOT_IN_SCOPE
#define HARDWARE TELEMETRIX_REASON_HARDWARE
#define NO_ANSWER TELEMETRIX_REASON_NO_ANSWER

#define PIN_ABSENT 99   /* not in the fixture profile */

static const telemetrix_i2c_bus_t fixed_i2c = {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000};
static const telemetrix_i2c_bus_t choose_i2c = {.bus = 0, .port = 0, .sda = -1, .scl = -1, .speed_hz = 100000};
static const telemetrix_onewire_bus_t fixed_ow = {.bus = 0, .gpio = 23};
static const telemetrix_onewire_bus_t choose_ow = {.bus = 0, .gpio = -1};

/** The reason of one stock command, straight from the dispatch. */
#define CMD(id, ...) \
    ({ \
        static const uint8_t p_[] = {__VA_ARGS__}; \
        tmx_adapter_command(&fx_a, (id), p_, sizeof(p_), 0); \
    })
#define CMD0(id) tmx_adapter_command(&fx_a, (id), NULL, 0, 0)

static telemetrix_reason_t pwm_mode(uint8_t pin, uint8_t res, double freq)
{
    uint8_t p[12] = {pin, TMX_MODE_PWM, 0, res};
    union {
        double d;
        uint64_t u;
    } v = {.d = freq};
    for (int i = 0; i < 8; i++) {
        p[4 + i] = (uint8_t)(v.u >> (8 * i));
    }
    return tmx_adapter_command(&fx_a, TMX_CMD_SET_PIN_MODE, p, sizeof(p), 0);
}

/* ---- the dispatch itself ---- */

TEST_CASE("command on a good payload: NONE", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_DIGITAL_WRITE, 12, 1));
}

TEST_CASE("command on a short payload: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_DIGITAL_WRITE, 12));
}

TEST_CASE("command on an id out of scope: NOT_IN_SCOPE", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NOT_IN_SCOPE, CMD(TMX_CMD_SPI_INIT, 1, 2, 3));
    TEST_ASSERT_EQUAL(NOT_IN_SCOPE, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_TOUCH));
}

TEST_CASE("command: does not count and does not emit upstream's refusal reports", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SERVO_ATTACH, PIN_ABSENT, 0, 0, 0, 0));
    fx_a.cfg.max_servo = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_SERVO_ATTACH, 12, 0x02, 0x00, 0x09, 0x00));
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_ignored());
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("command on LOOP: NONE and the reply is emitted by the command", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_LOOP_COMMAND, 7));
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("packet on a short payload: refused +1; on an out-of-scope id: ignored +1 only", "[adapter_reasons]")
{
    fx_start();
    FX_SEND(TMX_CMD_DIGITAL_WRITE, 0, 12);
    TEST_ASSERT_EQUAL(1, fx_refused());
    FX_SEND(TMX_CMD_SPI_INIT, 0, 1, 2, 3);
    TEST_ASSERT_EQUAL(1, fx_ignored());
    TEST_ASSERT_EQUAL(1, fx_refused());
}

/* ---- digital ---- */

TEST_CASE("SET_PIN_MODE digital on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SET_PIN_MODE, PIN_ABSENT, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SET_PIN_MODE, PIN_ABSENT, TMX_MODE_INPUT, 0));
}

TEST_CASE("SET_PIN_MODE digital on a pin without the capability: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_SET_PIN_MODE, 34, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_SET_PIN_MODE, 4, TMX_MODE_INPUT_PULLDOWN, 0));
}

TEST_CASE("SET_PIN_MODE digital on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_OUTPUT));
}

TEST_CASE("SET_PIN_MODE with an unknown mode: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_SET_PIN_MODE, 12, 8));
}

TEST_CASE("SET_PIN_MODE digital when the driver fails: HARDWARE, pin unclaimed", "[adapter_reasons]")
{
    fx_start();
    tmx_fake_fail_next("gpio_output", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("DIGITAL_WRITE on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_DIGITAL_WRITE, PIN_ABSENT, 1));
}

TEST_CASE("DIGITAL_WRITE on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_DIGITAL_WRITE, 12, 1));
}

TEST_CASE("DIGITAL_WRITE on a pin that is not an output: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_DIGITAL_WRITE, 12, 1));
}

TEST_CASE("MODIFY_REPORTING with an unknown mode: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_MODIFY_REPORTING, 5, 12));
}

TEST_CASE("MODIFY_REPORTING on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_MODIFY_REPORTING, 2, PIN_ABSENT));
}

TEST_CASE("MODIFY_REPORTING on a pin that is not in that mode: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_MODIFY_REPORTING, 2, 12));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_MODIFY_REPORTING, 1, 34));
}

/* ---- analog ---- */

TEST_CASE("SET_PIN_MODE analog on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SET_PIN_MODE, PIN_ABSENT, TMX_MODE_ANALOG, 0, 0, 1));
}

TEST_CASE("SET_PIN_MODE analog on a pin without an ADC: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_ANALOG, 0, 0, 1));
}

TEST_CASE("SET_PIN_MODE analog on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 34));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_SET_PIN_MODE, 34, TMX_MODE_ANALOG, 0, 0, 1));
}

TEST_CASE("SET_PIN_MODE analog on a good pin: NONE", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 34, TMX_MODE_ANALOG, 0, 0, 1));
}

/* ---- PWM ---- */

TEST_CASE("SET_PIN_MODE PWM on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, pwm_mode(PIN_ABSENT, 8, 5000.0));
}

TEST_CASE("SET_PIN_MODE PWM on a pin without PWM: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, pwm_mode(4, 8, 5000.0));
}

TEST_CASE("SET_PIN_MODE PWM on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(BUSY, pwm_mode(12, 8, 5000.0));
}

TEST_CASE("SET_PIN_MODE PWM with a resolution or frequency out of range: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, pwm_mode(12, 0, 5000.0));
    TEST_ASSERT_EQUAL(BAD_ARG, pwm_mode(12, 15, 5000.0));
    TEST_ASSERT_EQUAL(BAD_ARG, pwm_mode(12, 8, 0.5));
}

TEST_CASE("SET_PIN_MODE PWM with every LEDC channel granted gone: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_profile.ledc.channel_mask = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, pwm_mode(12, 8, 5000.0));
}

TEST_CASE("SET_PIN_MODE PWM with every LEDC timer taken: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_profile.ledc.timer_mask = 0x01;
    TEST_ASSERT_EQUAL(R_NONE, pwm_mode(12, 8, 5000.0));
    TEST_ASSERT_EQUAL(EXHAUSTED, pwm_mode(13, 8, 1000.0));
}

TEST_CASE("SET_PIN_MODE PWM when the timer or the attach fails: HARDWARE", "[adapter_reasons]")
{
    fx_start();
    tmx_fake_fail_next("ledc_timer", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, pwm_mode(12, 8, 5000.0));
    tmx_fake_fail_next("ledc_attach", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, pwm_mode(12, 8, 5000.0));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("ANALOG_WRITE on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_ANALOG_WRITE, PIN_ABSENT, 0, 5));
}

TEST_CASE("ANALOG_WRITE on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_ANALOG_WRITE, 12, 0, 5));
}

TEST_CASE("ANALOG_WRITE on a pin that is not PWM: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_ANALOG_WRITE, 12, 0, 5));
}

TEST_CASE("ANALOG_OUT_ATTACH and ANALOG_OUT_DETACH on a pin that is not PWM: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_ANALOG_OUT_ATTACH, 12, 0));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_ANALOG_OUT_DETACH, 12));
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_ANALOG_OUT_DETACH, PIN_ABSENT));
}

/* ---- servo ---- */

TEST_CASE("SERVO_ATTACH on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SERVO_ATTACH, PIN_ABSENT, 1, 0, 2, 0));
}

TEST_CASE("SERVO_ATTACH on a pin without PWM: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_SERVO_ATTACH, 4, 1, 0, 2, 0));
}

TEST_CASE("SERVO_ATTACH on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_SERVO_ATTACH, 12, 1, 0, 2, 0));
}

TEST_CASE("SERVO_ATTACH past max_servo: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_a.cfg.max_servo = 1;
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SERVO_ATTACH, 12, 1, 0x80, 2, 0));
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_SERVO_ATTACH, 13, 1, 0x80, 2, 0));
}

TEST_CASE("SERVO_ATTACH with no LEDC channel left: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_profile.ledc.channel_mask = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_SERVO_ATTACH, 12, 1, 0x80, 2, 0));
}

TEST_CASE("SERVO_ATTACH when the attach fails: HARDWARE", "[adapter_reasons]")
{
    fx_start();
    tmx_fake_fail_next("ledc_attach", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, CMD(TMX_CMD_SERVO_ATTACH, 12, 1, 0x80, 2, 0));
}

TEST_CASE("SERVO_WRITE and SERVO_DETACH on a pin that is not a servo: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_SERVO_WRITE, 12, 90));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_SERVO_DETACH, 12));
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SERVO_WRITE, PIN_ABSENT, 90));
}

/* ---- I2C ---- */

TEST_CASE("I2C commands on a profile without bus 0: BUS_UNKNOWN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, CMD0(TMX_CMD_I2C_BEGIN));
}

TEST_CASE("I2C_READ and I2C_WRITE before begin: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start_buses(&fixed_i2c, NULL);
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_I2C_READ, 0x76, 0xD0, 3, 1));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_I2C_WRITE, 1, 0x40, 0xAA));
}

TEST_CASE("I2C_BEGIN with [0,0] on a bus whose client chooses: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start_buses(&choose_i2c, NULL);
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_I2C_BEGIN, 0, 0));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD0(TMX_CMD_I2C_BEGIN));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_I2C_BEGIN, 21, 21));
}

TEST_CASE("I2C_BEGIN on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start_buses(&choose_i2c, NULL);
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_I2C_BEGIN, PIN_ABSENT, 22));
}

TEST_CASE("I2C_BEGIN on a pin without the I2C capability: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start_buses(&choose_i2c, NULL);
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_I2C_BEGIN, 21, 12));
}

TEST_CASE("I2C_BEGIN on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start_buses(&choose_i2c, NULL);
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 21, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_I2C_BEGIN, 21, 22));
}

TEST_CASE("I2C_BEGIN when the driver fails to open the bus: HARDWARE, pins released", "[adapter_reasons]")
{
    fx_start_buses(&fixed_i2c, NULL);
    tmx_fake_fail_next("i2c_open", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, CMD0(TMX_CMD_I2C_BEGIN));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 21));
}

TEST_CASE("I2C_READ the device does not answer: NO_ANSWER", "[adapter_reasons]")
{
    fx_start_buses(&fixed_i2c, NULL);
    TEST_ASSERT_EQUAL(R_NONE, CMD0(TMX_CMD_I2C_BEGIN));
    tmx_fake_fail_next("i2c_read", ESP_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL(NO_ANSWER, CMD(TMX_CMD_I2C_READ, 0x76, 0xD0, 3, 1));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("I2C_READ of 0 bytes: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start_buses(&fixed_i2c, NULL);
    TEST_ASSERT_EQUAL(R_NONE, CMD0(TMX_CMD_I2C_BEGIN));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_I2C_READ, 0x76, 0xD0, 0, 1));
}

TEST_CASE("I2C_WRITE the device NACKs: NO_ANSWER", "[adapter_reasons]")
{
    fx_start_buses(&fixed_i2c, NULL);
    TEST_ASSERT_EQUAL(R_NONE, CMD0(TMX_CMD_I2C_BEGIN));
    TEST_ASSERT_EQUAL(NO_ANSWER, CMD(TMX_CMD_I2C_WRITE, 1, 0x41, 0xAA));
}

/* ---- 1-wire ---- */

TEST_CASE("ONE_WIRE_INIT on a profile without bus 0: BUS_UNKNOWN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BUS_UNKNOWN, CMD(TMX_CMD_ONE_WIRE_INIT, 23));
}

TEST_CASE("ONE_WIRE commands before INIT: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start_buses(NULL, &fixed_ow);
    TEST_ASSERT_EQUAL(BAD_ARG, CMD0(TMX_CMD_ONE_WIRE_RESET));
    TEST_ASSERT_EQUAL(BAD_ARG, CMD0(TMX_CMD_ONE_WIRE_SEARCH));
}

TEST_CASE("ONE_WIRE_INIT on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start_buses(NULL, &choose_ow);
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_ONE_WIRE_INIT, PIN_ABSENT));
}

TEST_CASE("ONE_WIRE_INIT on a pin without the 1-wire capability: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start_buses(NULL, &choose_ow);
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_ONE_WIRE_INIT, 12));
}

TEST_CASE("ONE_WIRE_INIT on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start_buses(NULL, &fixed_ow);
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 23, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_ONE_WIRE_INIT, 23));
}

TEST_CASE("ONE_WIRE_INIT with no RMT RX channel granted: EXHAUSTED, pin unclaimed", "[adapter_reasons]")
{
    fx_start_buses(NULL, &fixed_ow);
    fx_profile.rmt.rx_channels = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_ONE_WIRE_INIT, 23));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 23));
}

TEST_CASE("ONE_WIRE_INIT when the driver fails to open: HARDWARE", "[adapter_reasons]")
{
    fx_start_buses(NULL, &fixed_ow);
    tmx_fake_fail_next("ow_open", ESP_FAIL);
    TEST_ASSERT_EQUAL(HARDWARE, CMD(TMX_CMD_ONE_WIRE_INIT, 23));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
}

/* ---- sonar and DHT ---- */

TEST_CASE("SONAR_NEW past max_sonar: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_a.cfg.max_sonar = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_SONAR_NEW, 12, 13));
}

TEST_CASE("SONAR_NEW with the same pin twice: BAD_ARGUMENT", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(BAD_ARG, CMD(TMX_CMD_SONAR_NEW, 12, 12));
}

TEST_CASE("SONAR_NEW on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SONAR_NEW, PIN_ABSENT, 13));
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_SONAR_NEW, 12, PIN_ABSENT));
}

TEST_CASE("SONAR_NEW with a trigger that cannot output: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_SONAR_NEW, 34, 13));
}

TEST_CASE("SONAR_NEW on a pin another use holds: PIN_BUSY, nothing claimed", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 13, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_SONAR_NEW, 12, 13));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
}

TEST_CASE("DHT_NEW past max_dht: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_a.cfg.max_dht = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_DHT_NEW, 12));
}

TEST_CASE("DHT_NEW with no RMT RX channel granted: EXHAUSTED", "[adapter_reasons]")
{
    fx_start();
    fx_profile.rmt.rx_channels = 0;
    TEST_ASSERT_EQUAL(EXHAUSTED, CMD(TMX_CMD_DHT_NEW, 12));
}

TEST_CASE("DHT_NEW on a pin not in the profile: NO_SUCH_PIN", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_PIN, CMD(TMX_CMD_DHT_NEW, PIN_ABSENT));
}

TEST_CASE("DHT_NEW on an input-only pin: NO_CAPABILITY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(NO_CAP, CMD(TMX_CMD_DHT_NEW, 34));
}

TEST_CASE("DHT_NEW on a pin another use holds: PIN_BUSY", "[adapter_reasons]")
{
    fx_start();
    TEST_ASSERT_EQUAL(R_NONE, CMD(TMX_CMD_SET_PIN_MODE, 12, TMX_MODE_OUTPUT));
    TEST_ASSERT_EQUAL(BUSY, CMD(TMX_CMD_DHT_NEW, 12));
}
