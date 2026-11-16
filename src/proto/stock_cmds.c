#include "tmx_proto.h"

/* Minimum payload length per command id; a negative entry is checked by the switch below. */
#define VARIABLE (-1)
#define OUT_OF_SCOPE (-2)

static int min_payload(uint8_t id)
{
    switch (id) {
    case TMX_CMD_LOOP_COMMAND: return 1;
    case TMX_CMD_SET_PIN_MODE: return VARIABLE;
    case TMX_CMD_DIGITAL_WRITE: return 2;
    case TMX_CMD_ANALOG_WRITE: return 3;
    case TMX_CMD_MODIFY_REPORTING: return 2;
    case TMX_CMD_GET_FIRMWARE_VERSION: return 0;
    case TMX_CMD_SERVO_ATTACH: return 5;
    case TMX_CMD_SERVO_WRITE: return 2;
    case TMX_CMD_SERVO_DETACH: return 1;
    case TMX_CMD_I2C_BEGIN: return VARIABLE;
    case TMX_CMD_I2C_READ: return 4;
    case TMX_CMD_I2C_WRITE: return VARIABLE;
    case TMX_CMD_SONAR_NEW: return 2;
    case TMX_CMD_DHT_NEW: return 1;
    case TMX_CMD_STOP_ALL_REPORTS: return 0;
    case TMX_CMD_SET_ANALOG_SCANNING_INTERVAL: return 1;
    case TMX_CMD_ENABLE_ALL_REPORTS: return 0;
    case TMX_CMD_ANALOG_OUT_ATTACH: return 2;
    case TMX_CMD_ANALOG_OUT_DETACH: return 1;
    case TMX_CMD_RESET: return 0;
    case TMX_CMD_ONE_WIRE_INIT: return 1;
    case TMX_CMD_ONE_WIRE_RESET: return 0;
    case TMX_CMD_ONE_WIRE_SELECT: return 8;
    case TMX_CMD_ONE_WIRE_SKIP: return 0;
    case TMX_CMD_ONE_WIRE_WRITE: return 2;
    case TMX_CMD_ONE_WIRE_READ: return 0;
    case TMX_CMD_ONE_WIRE_RESET_SEARCH: return 0;
    case TMX_CMD_ONE_WIRE_SEARCH: return 0;
    case TMX_CMD_ONE_WIRE_CRC8: return VARIABLE;
    default: return OUT_OF_SCOPE;   /* DAC, SPI, steppers, everything from 57 up */
    }
}

static size_t set_pin_mode_min(const uint8_t *payload, size_t len)
{
    if (len < 2) {
        return 2;
    }
    switch (payload[1]) {
    case TMX_MODE_INPUT:
    case TMX_MODE_INPUT_PULLUP:
    case TMX_MODE_INPUT_PULLDOWN: return 3;
    case TMX_MODE_ANALOG: return 5;
    case TMX_MODE_PWM: return 12;       /* pin, mode, channel, resolution, 8-byte frequency */
    default: return 2;                  /* output, and unknown modes the adapter refuses */
    }
}

tmx_stock_verdict_t tmx_stock_check(uint8_t id, const uint8_t *payload, size_t len)
{
    int min = min_payload(id);
    if (min == OUT_OF_SCOPE) {
        return TMX_STOCK_IGNORED;
    }
    size_t need = (size_t)(min > 0 ? min : 0);
    if (min == VARIABLE) {
        switch (id) {
        case TMX_CMD_SET_PIN_MODE:
            if (len >= 2 && payload[1] == TMX_MODE_TOUCH) {
                return TMX_STOCK_IGNORED;
            }
            need = set_pin_mode_min(payload, len);
            break;
        case TMX_CMD_I2C_BEGIN:
            need = len == 0 ? 0 : 2;                    /* [] or [sda, scl] */
            break;
        case TMX_CMD_I2C_WRITE:
            need = len < 1 ? 1 : 2 + (size_t)payload[0];    /* [count, addr, data x count] */
            break;
        default:    /* ONE_WIRE_CRC8 [n, bytes...]: the stock client sends n + 1 bytes, the extra is ignored */
            need = len < 1 ? 1 : 1 + (size_t)payload[0];
            break;
        }
    }
    return len >= need ? TMX_STOCK_OK : TMX_STOCK_SHORT;
}
