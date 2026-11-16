#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tmx_drv_fake.h"

#define FAKE_PINS 256
#define FAKE_BUSES 4
#define FAKE_I2C_DEVICES 8
#define FAKE_OW_ROMS 8
#define FAKE_LOG_SIZE 16384
#define FAKE_FAILS 16

typedef enum { MODE_NONE, MODE_INPUT, MODE_OUTPUT } pin_mode_t;

typedef struct {
    bool used;
    uint8_t addr;
    uint8_t regs[256];
    size_t n;
} i2c_device_t;

typedef struct {
    pin_mode_t mode[FAKE_PINS];
    bool pull_up[FAKE_PINS], pull_down[FAKE_PINS];
    bool out_level[FAKE_PINS];
    bool level[FAKE_PINS];
    int loop_src[FAKE_PINS];
    int adc[4][20];
    i2c_device_t i2c[FAKE_BUSES][FAKE_I2C_DEVICES];
    uint8_t dht_frame[FAKE_PINS][5];
    esp_err_t dht_err[FAKE_PINS];
    uint32_t sonar_us[FAKE_PINS];
    uint8_t roms[FAKE_BUSES][FAKE_OW_ROMS][8];
    size_t n_roms[FAKE_BUSES];
    size_t ow_next[FAKE_BUSES];
    char log[FAKE_LOG_SIZE];
    size_t log_len;
    bool log_truncated;
    struct {
        char op[24];
        esp_err_t err;
        bool pending;
    } fail[FAKE_FAILS];
} fake_t;

static fake_t fake;

static void record(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }
    if (fake.log_len + (size_t)n + 1 >= sizeof(fake.log)) {
        fake.log_truncated = true;
        return;
    }
    memcpy(fake.log + fake.log_len, line, (size_t)n);
    fake.log_len += (size_t)n;
    fake.log[fake.log_len++] = '\n';
    fake.log[fake.log_len] = '\0';
}

void tmx_fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    for (int i = 0; i < FAKE_PINS; i++) {
        fake.loop_src[i] = -1;
    }
}

void tmx_fake_set_level(uint8_t gpio, bool v)
{
    fake.level[gpio] = v;
}

void tmx_fake_loopback(uint8_t out_gpio, uint8_t in_gpio)
{
    fake.loop_src[in_gpio] = out_gpio;
}

void tmx_fake_set_adc(uint8_t unit, uint8_t channel, int raw)
{
    if (unit < 4 && channel < 20) {
        fake.adc[unit][channel] = raw;
    }
}

void tmx_fake_i2c_device(uint8_t bus, uint8_t addr, const uint8_t *regs, size_t n)
{
    if (bus >= FAKE_BUSES) {
        return;
    }
    for (int i = 0; i < FAKE_I2C_DEVICES; i++) {
        i2c_device_t *d = &fake.i2c[bus][i];
        if (!d->used || d->addr == addr) {
            d->used = true;
            d->addr = addr;
            d->n = n > sizeof(d->regs) ? sizeof(d->regs) : n;
            memcpy(d->regs, regs, d->n);
            return;
        }
    }
}

void tmx_fake_dht_frame(uint8_t gpio, const uint8_t f[5], esp_err_t err)
{
    memcpy(fake.dht_frame[gpio], f, 5);
    fake.dht_err[gpio] = err;
}

void tmx_fake_sonar_echo(uint8_t trig, uint32_t us)
{
    fake.sonar_us[trig] = us;
}

void tmx_fake_ow_devices(uint8_t bus, const uint8_t (*roms)[8], size_t n)
{
    if (bus >= FAKE_BUSES) {
        return;
    }
    fake.n_roms[bus] = n > FAKE_OW_ROMS ? FAKE_OW_ROMS : n;
    for (size_t i = 0; i < fake.n_roms[bus]; i++) {
        memcpy(fake.roms[bus][i], roms[i], 8);
    }
}

void tmx_fake_fail_next(const char *op, esp_err_t err)
{
    for (int i = 0; i < FAKE_FAILS; i++) {
        if (!fake.fail[i].pending) {
            strncpy(fake.fail[i].op, op, sizeof(fake.fail[i].op) - 1);
            fake.fail[i].op[sizeof(fake.fail[i].op) - 1] = '\0';
            fake.fail[i].err = err;
            fake.fail[i].pending = true;
            return;
        }
    }
}

/* The error a test queued for the next call of op, or ESP_OK. */
static esp_err_t injected(const char *op)
{
    for (int i = 0; i < FAKE_FAILS; i++) {
        if (fake.fail[i].pending && strcmp(fake.fail[i].op, op) == 0) {
            fake.fail[i].pending = false;
            return fake.fail[i].err;
        }
    }
    return ESP_OK;
}

bool tmx_fake_log_truncated(void)
{
    return fake.log_truncated;
}

const char *tmx_fake_calls(void)
{
    return fake.log;
}

size_t tmx_fake_count(const char *name)
{
    size_t len = strlen(name);
    size_t count = 0;
    for (const char *p = fake.log; *p;) {
        if (strncmp(p, name, len) == 0 && p[len] == '(') {
            count++;
        }
        const char *eol = strchr(p, '\n');
        p = eol ? eol + 1 : p + strlen(p);
    }
    return count;
}

static esp_err_t f_gpio_input(uint8_t gpio, bool pull_up, bool pull_down)
{
    record("gpio_input(%u,%d,%d)", gpio, pull_up, pull_down);
    esp_err_t inj = injected("gpio_input");
    if (inj != ESP_OK) {
        return inj;
    }
    fake.mode[gpio] = MODE_INPUT;
    fake.pull_up[gpio] = pull_up;
    fake.pull_down[gpio] = pull_down;
    return ESP_OK;
}

static esp_err_t f_gpio_output(uint8_t gpio, bool level)
{
    record("gpio_output(%u,%d)", gpio, level);
    esp_err_t inj = injected("gpio_output");
    if (inj != ESP_OK) {
        return inj;
    }
    fake.mode[gpio] = MODE_OUTPUT;
    fake.out_level[gpio] = level;
    return ESP_OK;
}

static void f_gpio_set(uint8_t gpio, bool v)
{
    record("gpio_set(%u,%d)", gpio, v);
    fake.out_level[gpio] = v;
}

static int f_gpio_get(uint8_t gpio)
{
    if (fake.mode[gpio] == MODE_OUTPUT) {
        return fake.out_level[gpio];
    }
    int src = fake.loop_src[gpio];
    if (src >= 0) {
        if (fake.mode[src] == MODE_OUTPUT) {
            return fake.out_level[src];
        }
        return fake.mode[gpio] == MODE_INPUT && fake.pull_up[gpio];
    }
    return fake.level[gpio];
}

static esp_err_t f_ledc_timer(uint8_t speed_mode, uint8_t timer, uint32_t freq_hz, uint8_t resolution)
{
    record("ledc_timer(%u,%u,%u,%u)", speed_mode, timer, (unsigned)freq_hz, resolution);
    esp_err_t inj = injected("ledc_timer");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static esp_err_t f_ledc_attach(uint8_t speed_mode, uint8_t channel, uint8_t timer, uint8_t gpio)
{
    record("ledc_attach(%u,%u,%u,%u)", speed_mode, channel, timer, gpio);
    esp_err_t inj = injected("ledc_attach");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static esp_err_t f_ledc_duty(uint8_t speed_mode, uint8_t channel, uint32_t duty)
{
    record("ledc_duty(%u,%u,%u)", speed_mode, channel, (unsigned)duty);
    esp_err_t inj = injected("ledc_duty");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static void f_ledc_stop(uint8_t speed_mode, uint8_t channel)
{
    record("ledc_stop(%u,%u)", speed_mode, channel);
}

static void f_ledc_timer_free(uint8_t speed_mode, uint8_t timer)
{
    record("ledc_timer_free(%u,%u)", speed_mode, timer);
}

static esp_err_t f_adc_read(uint8_t unit, uint8_t channel, int *raw)
{
    esp_err_t inj = injected("adc_read");
    if (inj != ESP_OK) {
        return inj;
    }
    if (unit >= 4 || channel >= 20) {
        return ESP_ERR_INVALID_ARG;
    }
    *raw = fake.adc[unit][channel];
    return ESP_OK;
}

static esp_err_t f_i2c_open(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl)
{
    (void)grant;
    record("i2c_open(%u,%d,%d)", bus, sda, scl);
    esp_err_t inj = injected("i2c_open");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static i2c_device_t *find_device(uint8_t bus, uint8_t addr)
{
    if (bus >= FAKE_BUSES) {
        return NULL;
    }
    for (int i = 0; i < FAKE_I2C_DEVICES; i++) {
        if (fake.i2c[bus][i].used && fake.i2c[bus][i].addr == addr) {
            return &fake.i2c[bus][i];
        }
    }
    return NULL;
}

static esp_err_t f_i2c_read(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n,
                            uint16_t timeout_ms)
{
    (void)timeout_ms;
    record("i2c_read(%u,%u,%u,%d,%u)", bus, addr, reg, stop, (unsigned)n);
    esp_err_t inj = injected("i2c_read");
    if (inj != ESP_OK) {
        return inj;
    }
    i2c_device_t *d = find_device(bus, addr);
    if (!d) {
        return ESP_FAIL;
    }
    if ((size_t)reg + n > d->n) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(data, d->regs + reg, n);
    return ESP_OK;
}

static esp_err_t f_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms)
{
    (void)data;
    (void)timeout_ms;
    record("i2c_write(%u,%u,%u)", bus, addr, (unsigned)n);
    esp_err_t inj = injected("i2c_write");
    if (inj != ESP_OK) {
        return inj;
    }
    return find_device(bus, addr) ? ESP_OK : ESP_FAIL;
}

static void f_i2c_close(uint8_t bus)
{
    record("i2c_close(%u)", bus);
}

static esp_err_t f_ow_open(uint8_t bus, uint8_t gpio)
{
    record("ow_open(%u,%u)", bus, gpio);
    esp_err_t inj = injected("ow_open");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static bool f_ow_reset(uint8_t bus)
{
    record("ow_reset(%u)", bus);
    return bus < FAKE_BUSES && fake.n_roms[bus] > 0;
}

static void f_ow_write(uint8_t bus, uint8_t byte, bool power)
{
    record("ow_write(%u,%u,%d)", bus, byte, power);
}

static uint8_t f_ow_read(uint8_t bus)
{
    record("ow_read(%u)", bus);
    return 0xff;
}

static bool f_ow_search(uint8_t bus, uint8_t rom[8])
{
    record("ow_search(%u)", bus);
    if (bus >= FAKE_BUSES || fake.ow_next[bus] >= fake.n_roms[bus]) {
        return false;
    }
    memcpy(rom, fake.roms[bus][fake.ow_next[bus]++], 8);
    return true;
}

static void f_ow_reset_search(uint8_t bus)
{
    record("ow_reset_search(%u)", bus);
    if (bus < FAKE_BUSES) {
        fake.ow_next[bus] = 0;
    }
}

static void f_ow_close(uint8_t bus)
{
    record("ow_close(%u)", bus);
}

static esp_err_t f_dht_start(uint8_t gpio, uint32_t token)
{
    record("dht_start(%u,%u)", gpio, (unsigned)token);
    esp_err_t inj = injected("dht_start");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static esp_err_t f_sonar_start(uint8_t trig, uint8_t echo, uint32_t token)
{
    record("sonar_start(%u,%u,%u)", trig, echo, (unsigned)token);
    esp_err_t inj = injected("sonar_start");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static esp_err_t f_init(const telemetrix_profile_t *p)
{
    (void)p;
    record("init()");
    esp_err_t inj = injected("init");
    if (inj != ESP_OK) {
        return inj;
    }
    return ESP_OK;
}

static void f_deinit(void)
{
    record("deinit()");
}

const tmx_drv_t tmx_drv_fake = {
    .gpio_input = f_gpio_input,
    .gpio_output = f_gpio_output,
    .gpio_set = f_gpio_set,
    .gpio_get = f_gpio_get,
    .ledc_timer = f_ledc_timer,
    .ledc_attach = f_ledc_attach,
    .ledc_duty = f_ledc_duty,
    .ledc_stop = f_ledc_stop,
    .ledc_timer_free = f_ledc_timer_free,
    .adc_read = f_adc_read,
    .i2c_open = f_i2c_open,
    .i2c_read = f_i2c_read,
    .i2c_write = f_i2c_write,
    .i2c_close = f_i2c_close,
    .ow_open = f_ow_open,
    .ow_reset = f_ow_reset,
    .ow_write = f_ow_write,
    .ow_read = f_ow_read,
    .ow_search = f_ow_search,
    .ow_reset_search = f_ow_reset_search,
    .ow_close = f_ow_close,
    .dht_start = f_dht_start,
    .sonar_start = f_sonar_start,
    .init = f_init,
    .deinit = f_deinit,
};
