/* libFuzzer target: the adapter on the fake driver with a full profile.
 * Input layout: byte 0 selects the profile variant (bit 0 I2C bus 0 fixed or chosen by the client, bit 1 1-wire
 * likewise, bit 2 no buses, bit 3 one RMT RX channel instead of two, bit 4 cfg.extended: a handshake packet can enter
 * extended mode), byte 1 the sensor limits; the rest is a list of
 * operations [op][args...]: a stock packet, a fake completion, scripted DHT and sonar results, an injected driver
 * failure, a clock step, a poll or a release. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "tmx_adapter.h"

#define N_PINS 40

static telemetrix_pin_t pins[N_PINS];
static int adc_handle;
static const telemetrix_adc_grant_t adc[] = {{.unit = 1, .handle = &adc_handle}};
static const telemetrix_i2c_bus_t i2c_fixed = {.bus = 0, .sda = 21, .scl = 22, .speed_hz = 100000};
static const telemetrix_i2c_bus_t i2c_choose = {.bus = 0, .sda = -1, .scl = -1, .speed_hz = 100000};
static const telemetrix_onewire_bus_t ow_fixed = {.bus = 0, .gpio = 23};
static const telemetrix_onewire_bus_t ow_choose = {.bus = 0, .gpio = -1};

static const char *const fail_ops[] = {
    "gpio_input", "gpio_output", "ledc_timer", "ledc_attach", "ledc_duty", "adc_read", "i2c_open", "i2c_read",
    "i2c_write", "ow_open", "dht_start", "sonar_start",
};

static size_t report_max;      /* TMX_REPORT_MAX for a stock-only input; extended reports reach 255 bytes */

static void emit(const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key, void *ctx)
{
    (void)reply;
    (void)kind;
    (void)key;
    if (n < 2 || n > report_max || report[0] != n - 1) {
        abort();
    }
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) {
        sink ^= report[i];
    }
    (*(uint32_t *)ctx)++;
}

static void build_profile(telemetrix_profile_t *p, uint8_t variant)
{
    for (int i = 0; i < N_PINS; i++) {
        pins[i] = (telemetrix_pin_t) {
            .gpio = (uint8_t)i,
            .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN |
                    TELEMETRIX_CAP_PWM | TELEMETRIX_CAP_I2C | TELEMETRIX_CAP_ONE_WIRE |
                    (i >= 32 ? TELEMETRIX_CAP_ADC : 0),
                    .safe = (telemetrix_safe_state_t)(i % 5),
                    .adc_unit = (int8_t)(i >= 32 ? 1 : -1),
                    .adc_channel = (int8_t)(i >= 32 ? i - 32 : -1),
        };
    }
    *p = (telemetrix_profile_t) {
        .pins = pins, .n_pins = N_PINS,
        .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
        .rmt = {.rx_channels = (variant & 8) ? 1 : 2, .tx_channels = 1},
        .adc = adc, .n_adc = 1,
    };
    if (!(variant & 4)) {
        p->i2c = (variant & 1) ? &i2c_fixed : &i2c_choose;
        p->n_i2c = 1;
        p->onewire = (variant & 2) ? &ow_fixed : &ow_choose;
        p->n_onewire = 1;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2) {
        return 0;
    }
    static telemetrix_profile_t profile;
    static tmx_adapter_t a;
    uint32_t emitted = 0;
    build_profile(&profile, data[0]);
    telemetrix_config_t cfg = {
        .i2c_timeout_ms = 50, .analog_interval_floor_ms = 10, .max_dht = data[1] & 7, .max_sonar = (data[1] >> 3) & 7,
        .max_servo = 4, .extended = (data[0] & 16) != 0,
    };
    tmx_fake_reset();
    tmx_completion_reset();
    report_max = cfg.extended ? TMX_EXT_REPORT_MAX - 1 : TMX_REPORT_MAX;
    if (tmx_adapter_init(&a, &profile, &cfg, &tmx_drv_fake, emit, &emitted) != ESP_OK) {
        abort();
    }
    uint8_t regs[256];
    for (int i = 0; i < 256; i++) {
        regs[i] = (uint8_t)i;
    }
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    uint32_t now = 0xFFFFF000u;     /* near the wrap of the millisecond counter */
    size_t pos = 2;
    while (pos < size) {
        uint8_t op = data[pos++] % 8;
        size_t left = size - pos;
        switch (op) {
        case 0: {       /* [0][id][len][payload...]: a stock packet, payload at most 30 bytes */
            if (left < 2) {
                return 0;
            }
            uint8_t id = data[pos] % 64;
            size_t len = data[pos + 1] % 31;
            pos += 2;
            if (len > size - pos) {
                len = size - pos;
            }
            tmx_adapter_packet(&a, id, data + pos, len, now);
            pos += len;
            break;
        }
        case 1:
            tmx_fake_complete_pending();
            break;
        case 2:         /* [2][gpio][5 frame bytes][err]: the DHT result of a pin */
            if (left < 7) {
                return 0;
            }
            tmx_fake_dht_frame(data[pos] % N_PINS, data + pos + 1, data[pos + 6] & 1 ? ESP_ERR_TIMEOUT : ESP_OK);
            pos += 7;
            break;
        case 3:         /* [3][trig][us hi][us lo]: the echo time of a sonar, 0 is a timeout */
            if (left < 3) {
                return 0;
            }
            tmx_fake_sonar_echo(data[pos] % N_PINS, (uint32_t)(data[pos + 1] << 8 | data[pos + 2]) * 4);
            pos += 3;
            break;
        case 4:
            if (left < 1) {
                return 0;
            }
            tmx_fake_fail_next(fail_ops[data[pos] % (sizeof(fail_ops) / sizeof(fail_ops[0]))], ESP_FAIL);
            pos++;
            break;
        case 5:         /* [5][step]: the clock advances step * 8 ms, sometimes by a lot */
            if (left < 1) {
                return 0;
            }
            now += (uint32_t)data[pos] * (data[pos] > 240 ? 1000u : 8u);
            pos++;
            break;
        case 6:
            tmx_adapter_poll(&a, now);
            break;
        default:
            if (data[pos - 1] & 8) {
                tmx_adapter_release(&a);
            } else {
                tmx_adapter_poll(&a, now);
            }
            break;
        }
        if (a.claims.rmt_rx_used > profile.rmt.rx_channels) {
            abort();
        }
    }
    tmx_adapter_release(&a);
    for (size_t i = 0; i < profile.n_pins; i++) {
        if (a.claims.use[i] != TMX_USE_NONE) {
            abort();
        }
    }
    if (a.claims.rmt_rx_used != 0) {
        abort();
    }
    return 0;
}
