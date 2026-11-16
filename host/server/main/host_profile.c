#include "fake/tmx_drv_fake.h"
#include "host_profile.h"

#define PLAIN (TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT | TELEMETRIX_CAP_PULL_UP | TELEMETRIX_CAP_PULL_DOWN)

#define PIN(g, c, s, u, ch) {.gpio = (g), .caps = (c), .safe = (s), .adc_unit = (u), .adc_channel = (ch)}

/* The pins the stock client accepts (4 5 12 13 14 16 17 18 19 21 22 23 25 26 27 32 33). gpio 5 rests low and gpio 13
 * rests high, so a test can tell the safe state from what a client left behind. Then the pins only an extended client
 * reaches: I2C bus 1 (2, 15), 1-wire bus 1 (35) and gpio 36, which carries the named capability "test.led" when the
 * test plugin is registered (the last pin, see host_profile). */
static telemetrix_pin_t pins[] = {
    PIN(4, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(5, PLAIN, TELEMETRIX_SAFE_DRIVE_LOW, -1, -1),
    PIN(12, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(13, PLAIN, TELEMETRIX_SAFE_DRIVE_HIGH, -1, -1),
    PIN(14, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(16, PLAIN | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(17, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(18, PLAIN | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(19, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(21, PLAIN | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(22, PLAIN | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(23, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(25, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(26, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(27, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(32, PLAIN | TELEMETRIX_CAP_ADC, TELEMETRIX_SAFE_FLOAT, 1, 4),
    PIN(33, PLAIN | TELEMETRIX_CAP_ADC, TELEMETRIX_SAFE_FLOAT, 1, 5),
    PIN(2, PLAIN | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(15, PLAIN | TELEMETRIX_CAP_I2C, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(35, PLAIN | TELEMETRIX_CAP_ONE_WIRE, TELEMETRIX_SAFE_FLOAT, -1, -1),
    PIN(36, PLAIN, TELEMETRIX_SAFE_FLOAT, -1, -1),
};

#define N_PINS (sizeof(pins) / sizeof(pins[0]))

static const char *const led_named[] = {"test.led"};

static const telemetrix_adc_grant_t adc[] = {{.unit = 1, .handle = NULL}};
static const telemetrix_i2c_bus_t i2c[] = {
    {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
    {.bus = 1, .port = 1, .sda = 2, .scl = 15, .speed_hz = 100000},
};
static const telemetrix_onewire_bus_t onewire[] = {{.bus = 0, .gpio = -1}, {.bus = 1, .gpio = 35}};

static const telemetrix_profile_t profile = {
    .pins = pins,
    .n_pins = N_PINS,
    .ledc = {.speed_mode = 0, .timer_mask = 0x0f, .channel_mask = 0xff},
    .rmt = {.rx_channels = 2, .tx_channels = 2},     /* each 1-wire bus claims a TX channel */
    .adc = adc,
    .n_adc = 1,
    .i2c = i2c,
    .n_i2c = 2,
    .onewire = onewire,
    .n_onewire = 2,
};

const telemetrix_profile_t *host_profile(bool test_plugin)
{
    /* a pin naming a capability no registered plugin declares makes telemetrix_start fail */
    pins[N_PINS - 1].named = test_plugin ? led_named : NULL;
    pins[N_PINS - 1].n_named = test_plugin ? 1 : 0;
    return &profile;
}

void host_script_fake(void)
{
    /* DHT22 frame: humidity 65.2 % (652), temperature 35.1 C (351), checksum 0x02 + 0x8c + 0x01 + 0x5f */
    static const uint8_t dht[5] = {0x02, 0x8c, 0x01, 0x5f, 0xee};
    /* family 0x28, serial 1..6, CRC-8/Maxim of the first seven bytes */
    static const uint8_t roms[1][8] = {{0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00}};
    uint8_t regs[256];
    uint8_t rom[1][8];

    for (int i = 0; i < 256; i++) {
        regs[i] = (uint8_t)(i ^ 0xa5);
    }
    for (int i = 0; i < 8; i++) {
        rom[0][i] = roms[0][i];
    }
    uint8_t crc = 0;
    for (int i = 0; i < 7; i++) {
        crc ^= rom[0][i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint8_t)((crc >> 1) ^ 0x8c) : (uint8_t)(crc >> 1);
        }
    }
    rom[0][7] = crc;

    tmx_fake_reset();
    tmx_fake_loopback(5, 4);
    tmx_fake_loopback(13, 12);
    tmx_fake_loopback(36, 35); /* the test plugin's LED claim (sub 3) reads back on pin 35 */
    tmx_fake_set_adc(1, 4, 2048);
    tmx_fake_set_adc(1, 5, 1024);
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    tmx_fake_i2c_device(1, 0x48, regs, sizeof(regs));
    tmx_fake_dht_frame(25, dht, ESP_OK);
    tmx_fake_sonar_echo(26, 5800);
    tmx_fake_ow_devices(0, (const uint8_t (*)[8])rom, 1);
    tmx_fake_ow_devices(1, (const uint8_t (*)[8])rom, 1);
}
