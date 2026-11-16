#include "tmx_proto.h"
#include "unity.h"

static const uint8_t zeros[64] = {0};

/* Every in-scope id with the exact payload length of the stock client. */
typedef struct {
    uint8_t id;
    uint8_t payload[16];
    size_t len;
} exact_t;

TEST_CASE("On each in-scope id with its exact payload length: returns TMX_STOCK_OK", "[stock_cmds]")
{
    static const exact_t table[] = {
        {TMX_CMD_LOOP_COMMAND, {1}, 1},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_INPUT, 1}, 3},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_OUTPUT}, 2},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_INPUT_PULLUP, 1}, 3},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_INPUT_PULLDOWN, 1}, 3},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_ANALOG, 0, 5, 1}, 5},
        {TMX_CMD_SET_PIN_MODE, {4, TMX_MODE_PWM, 0, 8, 0, 0, 0, 0, 0, 0, 0xE8, 0x40}, 12},
        {TMX_CMD_DIGITAL_WRITE, {4, 1}, 2},
        {TMX_CMD_ANALOG_WRITE, {4, 1, 0}, 3},
        {TMX_CMD_MODIFY_REPORTING, {2, 4}, 2},
        {TMX_CMD_GET_FIRMWARE_VERSION, {0}, 0},
        {TMX_CMD_SERVO_ATTACH, {4, 0x02, 0x58, 0x09, 0x60}, 5},
        {TMX_CMD_SERVO_WRITE, {4, 90}, 2},
        {TMX_CMD_SERVO_DETACH, {4}, 1},
        {TMX_CMD_I2C_BEGIN, {0}, 0},
        {TMX_CMD_I2C_BEGIN, {21, 22}, 2},
        {TMX_CMD_I2C_READ, {0x44, 0, 4, 1}, 4},
        {TMX_CMD_I2C_WRITE, {2, 0x44, 0xAA, 0xBB}, 4},
        {TMX_CMD_I2C_WRITE, {0, 0x44}, 2},
        {TMX_CMD_SONAR_NEW, {5, 18}, 2},
        {TMX_CMD_DHT_NEW, {4}, 1},
        {TMX_CMD_STOP_ALL_REPORTS, {0}, 0},
        {TMX_CMD_SET_ANALOG_SCANNING_INTERVAL, {19}, 1},
        {TMX_CMD_ENABLE_ALL_REPORTS, {0}, 0},
        {TMX_CMD_ANALOG_OUT_ATTACH, {4, 0}, 2},
        {TMX_CMD_ANALOG_OUT_DETACH, {4}, 1},
        {TMX_CMD_RESET, {0}, 0},
        {TMX_CMD_ONE_WIRE_INIT, {4}, 1},
        {TMX_CMD_ONE_WIRE_RESET, {0}, 0},
        {TMX_CMD_ONE_WIRE_SELECT, {0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x6D}, 8},
        {TMX_CMD_ONE_WIRE_SKIP, {0}, 0},
        {TMX_CMD_ONE_WIRE_WRITE, {0x44, 0}, 2},
        {TMX_CMD_ONE_WIRE_READ, {0}, 0},
        {TMX_CMD_ONE_WIRE_RESET_SEARCH, {0}, 0},
        {TMX_CMD_ONE_WIRE_SEARCH, {0}, 0},
        /* stock client, 8-byte address: [n = 7, 8 bytes] */
        {TMX_CMD_ONE_WIRE_CRC8, {7, 0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x6D}, 9},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(TMX_STOCK_OK, tmx_stock_check(table[i].id, table[i].payload, table[i].len),
                                  "in-scope id with exact payload");
    }
}

TEST_CASE("On DIGITAL_WRITE with 1 payload byte: returns TMX_STOCK_SHORT", "[stock_cmds]")
{
    const uint8_t p[] = {4};
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_DIGITAL_WRITE, p, 1));
}

TEST_CASE("On an empty payload for a command that needs one: returns TMX_STOCK_SHORT", "[stock_cmds]")
{
    static const uint8_t ids[] = {TMX_CMD_LOOP_COMMAND, TMX_CMD_SET_PIN_MODE, TMX_CMD_ANALOG_WRITE,
                                  TMX_CMD_SERVO_ATTACH, TMX_CMD_I2C_READ, TMX_CMD_I2C_WRITE, TMX_CMD_SONAR_NEW,
                                  TMX_CMD_DHT_NEW, TMX_CMD_ONE_WIRE_INIT, TMX_CMD_ONE_WIRE_SELECT,
                                  TMX_CMD_ONE_WIRE_CRC8
                                 };
    for (size_t i = 0; i < sizeof(ids); i++) {
        TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(ids[i], zeros, 0));
    }
}

TEST_CASE("On SET_PIN_MODE whose mode needs more bytes: returns TMX_STOCK_SHORT", "[stock_cmds]")
{
    const uint8_t input[] = {4, TMX_MODE_INPUT};
    const uint8_t analog[] = {4, TMX_MODE_ANALOG, 0, 5};
    const uint8_t pwm[] = {4, TMX_MODE_PWM, 0, 8, 0, 0, 0, 0, 0, 0, 0xE8};
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_SET_PIN_MODE, input, sizeof(input)));
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_SET_PIN_MODE, analog, sizeof(analog)));
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_SET_PIN_MODE, pwm, sizeof(pwm)));
}

TEST_CASE("On I2C_BEGIN with a single byte: returns TMX_STOCK_SHORT", "[stock_cmds]")
{
    const uint8_t p[] = {21};
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_I2C_BEGIN, p, 1));
}

TEST_CASE("On I2C_WRITE whose count exceeds the payload: returns TMX_STOCK_SHORT", "[stock_cmds]")
{
    const uint8_t p[] = {3, 0x44, 0xAA, 0xBB};
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_I2C_WRITE, p, sizeof(p)));
}

TEST_CASE("On ONE_WIRE_CRC8 [n, n+1 bytes] (stock client): returns TMX_STOCK_OK (extra byte ignored)", "[stock_cmds]")
{
    /* onewire_crc8([0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, crc]) sends [35, 7, <8 bytes>] */
    const uint8_t p[] = {7, 0x28, 0xFF, 0x4C, 0x61, 0x92, 0x16, 0x05, 0x6D};
    TEST_ASSERT_EQUAL(TMX_STOCK_OK, tmx_stock_check(TMX_CMD_ONE_WIRE_CRC8, p, sizeof(p)));
    TEST_ASSERT_EQUAL(TMX_STOCK_OK, tmx_stock_check(TMX_CMD_ONE_WIRE_CRC8, p, sizeof(p) - 1));
    TEST_ASSERT_EQUAL(TMX_STOCK_SHORT, tmx_stock_check(TMX_CMD_ONE_WIRE_CRC8, p, sizeof(p) - 2));
}

TEST_CASE("On ids 19, 21, 22-26, 36-56, 57-255 and SET_PIN_MODE mode 7: returns TMX_STOCK_IGNORED", "[stock_cmds]")
{
    for (int id = 0; id <= 255; id++) {
        bool ignored = id == 19 || id == 21 || (id >= 22 && id <= 26) || id >= 36;
        if (ignored) {
            TEST_ASSERT_EQUAL_MESSAGE(TMX_STOCK_IGNORED, tmx_stock_check((uint8_t)id, zeros, 0), "out of scope id");
            TEST_ASSERT_EQUAL_MESSAGE(TMX_STOCK_IGNORED, tmx_stock_check((uint8_t)id, zeros, 30), "with payload");
        }
    }
    const uint8_t touch[] = {4, TMX_MODE_TOUCH, 1};
    TEST_ASSERT_EQUAL(TMX_STOCK_IGNORED, tmx_stock_check(TMX_CMD_SET_PIN_MODE, touch, sizeof(touch)));
}

TEST_CASE("On a non-bus command with trailing extra bytes: returns TMX_STOCK_OK (as upstream)", "[stock_cmds]")
{
    static const uint8_t p[] = {4, 1, 9, 9, 9};
    TEST_ASSERT_EQUAL(TMX_STOCK_OK, tmx_stock_check(TMX_CMD_DIGITAL_WRITE, p, sizeof(p)));
    TEST_ASSERT_EQUAL(TMX_STOCK_OK, tmx_stock_check(TMX_CMD_GET_FIRMWARE_VERSION, p, sizeof(p)));
    TEST_ASSERT_EQUAL(TMX_STOCK_OK, tmx_stock_check(TMX_CMD_SERVO_DETACH, p, sizeof(p)));
}
