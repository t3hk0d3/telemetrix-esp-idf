#include <string.h>

#include "tmx_proto.h"

#define I2C_DATA_MAX 59
#define ONEWIRE_DATA_MAX (TMX_REPORT_MAX - 3)

static size_t put(uint8_t *out, uint8_t id, const uint8_t *payload, size_t n)
{
    out[0] = (uint8_t)(n + 1);
    out[1] = id;
    if (n > 0) {
        memcpy(out + 2, payload, n);
    }
    return n + 2;
}

static void put_float_le(uint8_t *out, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(bits >> (8 * i));
    }
}

size_t tmx_rpt_loop(uint8_t *out, uint8_t c)
{
    return put(out, TMX_CMD_LOOP_COMMAND, &c, 1);
}

size_t tmx_rpt_digital(uint8_t *out, uint8_t pin, uint8_t v)
{
    const uint8_t p[] = {pin, v};
    return put(out, TMX_RPT_DIGITAL_REPORT, p, sizeof(p));
}

size_t tmx_rpt_analog(uint8_t *out, uint8_t pin, uint16_t value)
{
    const uint8_t p[] = {pin, (uint8_t)(value >> 8), (uint8_t)value};
    return put(out, TMX_RPT_ANALOG_REPORT, p, sizeof(p));
}

size_t tmx_rpt_firmware(uint8_t *out)
{
    static const uint8_t p[] = {3, 2, 100};
    return put(out, TMX_RPT_FIRMWARE_REPORT, p, sizeof(p));
}

size_t tmx_rpt_servo_unavailable(uint8_t *out, uint8_t pin)
{
    return put(out, TMX_RPT_SERVO_UNAVAILABLE, &pin, 1);
}

size_t tmx_rpt_i2c_read(uint8_t *out, uint8_t addr, uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t p[3 + I2C_DATA_MAX];
    if (n > I2C_DATA_MAX) {
        n = I2C_DATA_MAX;
    }
    p[0] = (uint8_t)n;
    p[1] = addr;
    p[2] = reg;
    if (n > 0) {
        memcpy(p + 3, data, n);
    }
    return put(out, TMX_RPT_I2C_READ_REPORT, p, 3 + n);
}

size_t tmx_rpt_i2c_short(uint8_t *out, uint8_t addr)
{
    const uint8_t p[] = {1, addr};
    return put(out, TMX_RPT_I2C_TOO_MANY_BYTES_RCVD, p, sizeof(p));
}

size_t tmx_rpt_sonar(uint8_t *out, uint8_t trig, uint16_t cm)
{
    const uint8_t p[] = {trig, (uint8_t)(cm >> 8), (uint8_t)cm};
    return put(out, TMX_RPT_SONAR_DISTANCE, p, sizeof(p));
}

size_t tmx_rpt_dht_data(uint8_t *out, uint8_t pin, float hum, float temp)
{
    uint8_t p[10] = {0, pin};
    put_float_le(p + 2, hum);
    put_float_le(p + 6, temp);
    return put(out, TMX_RPT_DHT_REPORT, p, sizeof(p));
}

size_t tmx_rpt_dht_error(uint8_t *out, uint8_t pin, uint8_t err)
{
    const uint8_t p[] = {1, pin, err};
    return put(out, TMX_RPT_DHT_REPORT, p, sizeof(p));
}

size_t tmx_rpt_onewire(uint8_t *out, uint8_t subtype, const uint8_t *data, size_t n)
{
    uint8_t p[1 + ONEWIRE_DATA_MAX];
    if (n > ONEWIRE_DATA_MAX) {
        n = ONEWIRE_DATA_MAX;
    }
    p[0] = subtype;
    if (n > 0) {
        memcpy(p + 1, data, n);
    }
    return put(out, TMX_RPT_ONE_WIRE_REPORT, p, 1 + n);
}
