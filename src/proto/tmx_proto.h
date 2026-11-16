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

/* Extended-mode command ids (this server's extension protocol). */
#define TMX_CMD_SYNC 57
#define TMX_CMD_ON_BUS 58
#define TMX_CMD_GET_CAPABILITIES 59
#define TMX_CMD_PLUGIN 60
#define TMX_CMD_EXT_LAST 60

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

/* Extended-mode report ids. */
#define TMX_RPT_ERROR 57
#define TMX_RPT_SYNC_REPLY 58
#define TMX_RPT_BUS_REPORT 59
#define TMX_RPT_CAPABILITIES 60
#define TMX_RPT_PLUGIN_REPORT 61

/* Extension handshake: magic byte and protocol version, payload and report caps in extended mode. */
#define TMX_EXT_MAGIC 0xA7
#define TMX_EXT_MAJOR 1
#define TMX_EXT_MINOR 0
#define TMX_EXT_PAYLOAD_MAX 254
#define TMX_EXT_REPORT_MAX 256

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
    bool strict;          /**< extended mode: len 0 and a timed-out packet are violations */
    bool violation;       /**< a violation was seen; feed consumes nothing until the next init */
} tmx_frame_t;

/** Reset the assembler; payload_cap is 30 in stock mode (clamped to 254). dropped restarts at 0. */
void tmx_frame_init(tmx_frame_t *f, uint16_t payload_cap);
/** Feed received bytes; fn is called once per complete packet with the payload inside f's buffer. */
void tmx_frame_feed(tmx_frame_t *f, const uint8_t *data, size_t n, uint32_t now_ms, tmx_packet_fn fn, void *ctx);
/** Drop a packet that has been unfinished for more than 2000 ms and count it; an expired skip only stops discarding
 *  (the oversized packet was counted when it began). */
void tmx_frame_tick(tmx_frame_t *f, uint32_t now_ms);
/** The connection ended: drop an unfinished packet and count it; an unfinished skip is forgotten (already counted). */
void tmx_frame_abandon(tmx_frame_t *f);
/** Change the payload cap (clamped to 254) and strictness. Takes effect at the next length byte; callable from inside
 *  the packet callback. */
void tmx_frame_set_mode(tmx_frame_t *f, uint16_t payload_cap, bool strict);
/** Stop parsing: sets violation so feed returns without emitting the rest of the chunk. Callable from the callback. */
void tmx_frame_halt(tmx_frame_t *f);

typedef enum { TMX_STOCK_OK, TMX_STOCK_SHORT, TMX_STOCK_IGNORED } tmx_stock_verdict_t;

/** Check a stock packet's payload length. Does not validate pins or values; trailing bytes are tolerated. */
tmx_stock_verdict_t tmx_stock_check(uint8_t id, const uint8_t *payload, size_t len);

/* Reports sent to the client. Each encoder writes [len][id][payload] into out and returns the bytes written; out must
 * hold TMX_REPORT_MAX bytes. */
#define TMX_REPORT_MAX 64

size_t tmx_rpt_loop(uint8_t *out, uint8_t c);
size_t tmx_rpt_digital(uint8_t *out, uint8_t pin, uint8_t v);
size_t tmx_rpt_analog(uint8_t *out, uint8_t pin, uint16_t value);
size_t tmx_rpt_firmware(uint8_t *out);
/** [2,6,pin]; upstream omits the length byte, this server sends it (README deviation). */
size_t tmx_rpt_servo_unavailable(uint8_t *out, uint8_t pin);
/** n is clamped to 59 so the report fits TMX_REPORT_MAX. */
size_t tmx_rpt_i2c_read(uint8_t *out, uint8_t addr, uint8_t reg, const uint8_t *data, size_t n);
/** [3,8,1,addr]: a NACK or a short read. */
size_t tmx_rpt_i2c_short(uint8_t *out, uint8_t addr);
size_t tmx_rpt_sonar(uint8_t *out, uint8_t trig, uint16_t cm);
/** hum and temp are encoded as little-endian IEEE-754 floats regardless of host byte order. */
size_t tmx_rpt_dht_data(uint8_t *out, uint8_t pin, float hum, float temp);
/** err: 1 checksum, 2 timeout. */
size_t tmx_rpt_dht_error(uint8_t *out, uint8_t pin, uint8_t err);
/** [1+1+n,14,subtype,data...]; subtype is a TMX_CMD_ONE_WIRE_* id; n is 1 (reset, read, crc8) or 8 (search). */
size_t tmx_rpt_onewire(uint8_t *out, uint8_t subtype, const uint8_t *data, size_t n);

/* Extended reports; out must hold TMX_EXT_REPORT_MAX bytes. Each returns the bytes written. */

/** [7,5,3,2,100,magic,major,minor]: the stock version bytes then the server's extension version. */
size_t tmx_rpt_firmware_long(uint8_t *out, uint8_t major, uint8_t minor);
/** [len,57,seq_hi,seq_lo,cmd,reason], then [slot,sub] when slot >= 0, then [code] when code >= 0 (only after a slot).
 *  The caller passes slot >= 0 exactly when cmd is TMX_CMD_PLUGIN and code >= 0 exactly when reason is PLUGIN. */
size_t tmx_rpt_error(uint8_t *out, uint16_t seq, uint8_t cmd, uint8_t reason, int16_t slot, uint8_t sub, int16_t code);
/** [5,58,tag_hi,tag_lo,seq_hi,seq_lo] */
size_t tmx_rpt_sync_reply(uint8_t *out, uint16_t tag, uint16_t seq);
/** Wrap a full stock report (length byte included) as [len,59,bus,stock_id,payload...]. Returns 0 when it does not
 *  fit or is malformed. */
size_t tmx_rpt_bus_wrap(uint8_t *out, uint8_t bus, const uint8_t *stock_report, size_t n);
/** [len,61,slot,sub,data...]; returns 0 when n is above 251. */
size_t tmx_rpt_plugin(uint8_t *out, uint8_t slot, uint8_t sub, const uint8_t *data, size_t n);

/* ---- CAPABILITIES records and pages (caps.c) ---- */

/** Record types. A record is [type][len][value], len counting the value only. */
#define TMX_CAPS_CHIP 1
#define TMX_CAPS_PIN 2
#define TMX_CAPS_BUS 3
#define TMX_CAPS_LIMIT 4
#define TMX_CAPS_PLUGIN 5
#define TMX_CAPS_NAMED 6

/** BUS record kinds and LIMIT kinds. */
#define TMX_CAPS_BUS_I2C 0
#define TMX_CAPS_BUS_ONEWIRE 1
#define TMX_CAPS_LIMIT_SERVOS 1
#define TMX_CAPS_LIMIT_PWM_CHANNELS 2
#define TMX_CAPS_LIMIT_PWM_TIMERS 3
#define TMX_CAPS_LIMIT_DHT 4
#define TMX_CAPS_LIMIT_SONAR 5
#define TMX_CAPS_LIMIT_ANALOG_FLOOR_MS 6

/** Record bytes one CAPABILITIES page carries: the report is [len,60,page,last_page,records...], at most 255 bytes. */
#define TMX_CAPS_PAGE_MAX 251
/** The most a record can hold, [type][len] included: a record never spans pages (strings are cut to fit). */
#define TMX_CAPS_RECORD_MAX TMX_CAPS_PAGE_MAX

/* Each encoder writes one record into out (TMX_CAPS_RECORD_MAX bytes) and returns its length. Multi-byte values are
 * big-endian and signed bytes two's complement. Strings are ASCII, not NUL-terminated, and cut when the record would
 * not fit in a page. */

/** [model u16][revision u16][idf ASCII] */
size_t tmx_caps_chip(uint8_t *out, uint16_t model, uint16_t revision, const char *idf);
/** [gpio][caps u16][safe][adc_unit i8][adc_channel i8][named cap ids...] */
size_t tmx_caps_pin(uint8_t *out, uint8_t gpio, uint16_t caps, uint8_t safe, int8_t adc_unit, int8_t adc_channel,
                    const uint8_t *named_ids, size_t n_named);
/** [0][bus][sda i8][scl i8][speed_hz u32] */
size_t tmx_caps_bus_i2c(uint8_t *out, uint8_t bus, int8_t sda, int8_t scl, uint32_t speed_hz);
/** [1][bus][gpio i8] */
size_t tmx_caps_bus_onewire(uint8_t *out, uint8_t bus, int8_t gpio);
/** [kind][count u16] */
size_t tmx_caps_limit(uint8_t *out, uint8_t kind, uint16_t count);
/** [slot][major][minor][name ASCII] */
size_t tmx_caps_plugin(uint8_t *out, uint8_t slot, uint8_t major, uint8_t minor, const char *name);
/** [id][name ASCII] */
size_t tmx_caps_named(uint8_t *out, uint8_t id, const char *name);

/** Yields record i into rec (TMX_CAPS_RECORD_MAX bytes) and returns its length, 0 after the last. */
typedef size_t (*tmx_caps_source_fn)(size_t i, uint8_t *rec, void *ctx);
/** Write the whole CAPABILITIES report [len,60,page,last_page,records...] of a page into out (TMX_EXT_REPORT_MAX bytes)
 *  and its length into *n. Records are packed in order, never split, at most TMX_CAPS_PAGE_MAX bytes a page. The pages
 *  are found by walking the source from record 0 on every call. False when page is past the last one. */
bool tmx_caps_page(tmx_caps_source_fn src, void *ctx, uint8_t page, uint8_t *out, size_t *n);

#endif
