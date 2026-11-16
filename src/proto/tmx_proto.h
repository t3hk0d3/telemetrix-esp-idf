#ifndef TMX_PROTO_H
#define TMX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetrix.h"

/* Command ids, upstream private_constants.py (Telemetrix4Esp32 3.2.0). */
#define TMX_CMD_LOOP_COMMAND 0
#define TMX_CMD_SET_PIN_MODE 1
#define TMX_CMD_DIGITAL_WRITE 2
#define TMX_CMD_ANALOG_WRITE 3
#define TMX_CMD_MODIFY_REPORTING 4
#define TMX_CMD_GET_FIRMWARE_VERSION 5
#define TMX_CMD_SERVO_ATTACH 6
#define TMX_CMD_SERVO_WRITE 7
#define TMX_CMD_SERVO_DETACH 8
#define TMX_CMD_I2C_BEGIN 9
#define TMX_CMD_I2C_READ 10
#define TMX_CMD_I2C_WRITE 11
#define TMX_CMD_SONAR_NEW 12
#define TMX_CMD_DHT_NEW 13
#define TMX_CMD_STOP_ALL_REPORTS 14
#define TMX_CMD_SET_ANALOG_SCANNING_INTERVAL 15
#define TMX_CMD_ENABLE_ALL_REPORTS 16
#define TMX_CMD_ANALOG_OUT_ATTACH 17
#define TMX_CMD_ANALOG_OUT_DETACH 18
#define TMX_CMD_DAC_WRITE 19
#define TMX_CMD_RESET 20
#define TMX_CMD_DAC_DISABLE 21
#define TMX_CMD_SPI_INIT 22
#define TMX_CMD_SPI_WRITE_BLOCKING 23
#define TMX_CMD_SPI_READ_BLOCKING 24
#define TMX_CMD_SPI_SET_FORMAT 25
#define TMX_CMD_SPI_CS_CONTROL 26
#define TMX_CMD_ONE_WIRE_INIT 27
#define TMX_CMD_ONE_WIRE_RESET 28
#define TMX_CMD_ONE_WIRE_SELECT 29
#define TMX_CMD_ONE_WIRE_SKIP 30
#define TMX_CMD_ONE_WIRE_WRITE 31
#define TMX_CMD_ONE_WIRE_READ 32
#define TMX_CMD_ONE_WIRE_RESET_SEARCH 33
#define TMX_CMD_ONE_WIRE_SEARCH 34
#define TMX_CMD_ONE_WIRE_CRC8 35
#define TMX_CMD_SET_PIN_MODE_STEPPER 36
#define TMX_CMD_STEPPER_MOVE_TO 37
#define TMX_CMD_STEPPER_MOVE 38
#define TMX_CMD_STEPPER_RUN 39
#define TMX_CMD_STEPPER_RUN_SPEED 40
#define TMX_CMD_STEPPER_SET_MAX_SPEED 41
#define TMX_CMD_STEPPER_SET_ACCELERATION 42
#define TMX_CMD_STEPPER_SET_SPEED 43
#define TMX_CMD_STEPPER_SET_CURRENT_POSITION 44
#define TMX_CMD_STEPPER_RUN_SPEED_TO_POSITION 45
#define TMX_CMD_STEPPER_STOP 46
#define TMX_CMD_STEPPER_DISABLE_OUTPUTS 47
#define TMX_CMD_STEPPER_ENABLE_OUTPUTS 48
#define TMX_CMD_STEPPER_SET_MINIMUM_PULSE_WIDTH 49
#define TMX_CMD_STEPPER_SET_ENABLE_PIN 50
#define TMX_CMD_STEPPER_SET_3_PINS_INVERTED 51
#define TMX_CMD_STEPPER_SET_4_PINS_INVERTED 52
#define TMX_CMD_STEPPER_IS_RUNNING 53
#define TMX_CMD_STEPPER_GET_CURRENT_POSITION 54
#define TMX_CMD_STEPPER_GET_DISTANCE_TO_GO 55
#define TMX_CMD_STEPPER_GET_TARGET_POSITION 56

/* Report ids sent to the client. */
#define TMX_RPT_DIGITAL_REPORT 2
#define TMX_RPT_ANALOG_REPORT 3
#define TMX_RPT_FIRMWARE_REPORT 5
#define TMX_RPT_SERVO_UNAVAILABLE 6
#define TMX_RPT_I2C_TOO_FEW_BYTES_RCVD 7
#define TMX_RPT_I2C_TOO_MANY_BYTES_RCVD 8
#define TMX_RPT_I2C_READ_REPORT 9
#define TMX_RPT_SONAR_DISTANCE 10
#define TMX_RPT_DHT_REPORT 11
#define TMX_RPT_TOUCH_REPORT 12
#define TMX_RPT_SPI_REPORT 13
#define TMX_RPT_ONE_WIRE_REPORT 14
#define TMX_RPT_STEPPER_DISTANCE_TO_GO 15
#define TMX_RPT_STEPPER_TARGET_POSITION 16
#define TMX_RPT_STEPPER_CURRENT_POSITION 17
#define TMX_RPT_STEPPER_RUNNING_REPORT 18
#define TMX_RPT_STEPPER_RUN_COMPLETE_REPORT 19
#define TMX_RPT_DEBUG_PRINT 99

/* SET_PIN_MODE modes. */
#define TMX_MODE_INPUT 0
#define TMX_MODE_OUTPUT 1
#define TMX_MODE_INPUT_PULLUP 2
#define TMX_MODE_ANALOG 3
#define TMX_MODE_TOUCH 7
#define TMX_MODE_PWM 8
#define TMX_MODE_INPUT_PULLDOWN 9

/* Stock framing: payload cap 30, so len 2..31 carries a command with payload and len 1 a command alone. */
#define TMX_STOCK_PAYLOAD_MAX 30
/* A packet whose bytes do not all arrive within this time is dropped. */
#define TMX_FRAME_TIMEOUT_MS 2000

typedef void (*tmx_packet_fn)(uint8_t id, const uint8_t *payload, size_t len, void *ctx);

typedef struct {
    uint8_t buf[256];     /**< id followed by the payload of the packet being assembled */
    uint16_t need;        /**< len of the packet being assembled (id + payload), 0 between packets */
    uint16_t have;        /**< bytes of it received so far */
    uint16_t skip;        /**< bytes of an oversized packet still to discard */
    uint32_t started_ms;  /**< time the first byte of the current packet or skip arrived */
    uint32_t dropped;     /**< len 0, oversized and timed-out packets */
    uint16_t cap;         /**< payload cap: len above cap + 1 is skipped */
} tmx_frame_t;

/** Reset the assembler; payload_cap is 30 in stock mode (clamped to 254). dropped restarts at 0. */
void tmx_frame_init(tmx_frame_t *f, uint16_t payload_cap);
/** Feed received bytes; fn is called once per complete packet with the payload inside f's buffer. */
void tmx_frame_feed(tmx_frame_t *f, const uint8_t *data, size_t n, uint32_t now_ms, tmx_packet_fn fn, void *ctx);
/** Drop a packet (or skip) that has been unfinished for more than 2000 ms; counts a dropped packet. */
void tmx_frame_tick(tmx_frame_t *f, uint32_t now_ms);

typedef enum { TMX_STOCK_OK, TMX_STOCK_SHORT, TMX_STOCK_IGNORED } tmx_stock_verdict_t;

/** Check a stock packet's payload length. Does not validate pins or values; trailing bytes are tolerated. */
tmx_stock_verdict_t tmx_stock_check(uint8_t id, const uint8_t *payload, size_t len);

#endif
