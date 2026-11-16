#include "driver/i2c_master.h"
#include "esp_log.h"

#include "tmx_drv_esp.h"

static const char *TAG = "telemetrix";

#define I2C_BUSES TMX_MAX_BUSES
#define I2C_DEVICES 8   /* cached device handles per bus */

typedef struct {
    bool open;
    bool owned;         /* the driver created the bus handle and deletes it; an application-owned one is only left */
    i2c_master_bus_handle_t bus;
    uint32_t speed_hz;
    int8_t sda, scl;
    uint8_t n_dev;
    struct {
        uint8_t addr;
        i2c_master_dev_handle_t handle;
    } dev[I2C_DEVICES];
} i2c_bus_t;

static i2c_bus_t buses[I2C_BUSES];

static esp_err_t fail(esp_err_t err)
{
    return err == ESP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;   /* a NACK is ESP_FAIL */
}

static int timeout_of(uint16_t timeout_ms)
{
    return timeout_ms > 0 ? timeout_ms : 1;   /* -1 would wait for ever */
}

esp_err_t esp_i2c_open(uint8_t bus, const telemetrix_i2c_bus_t *grant, int8_t sda, int8_t scl)
{
    if (bus >= I2C_BUSES || !grant) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus_t *b = &buses[bus];
    if (b->open) {
        return ESP_ERR_INVALID_STATE;
    }
    *b = (i2c_bus_t) {
        .speed_hz = grant->speed_hz, .sda = -1, .scl = -1
    };
    if (grant->handle) {
        b->bus = (i2c_master_bus_handle_t)grant->handle;   /* the application's pins: not ours to touch, not even cached */
    } else {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = grant->port,
            .sda_io_num = (gpio_num_t)sda,
            .scl_io_num = (gpio_num_t)scl,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t err = i2c_new_master_bus(&cfg, &b->bus);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2c bus %u on port %u: %s", bus, grant->port, esp_err_to_name(err));
            return err;
        }
        b->owned = true;
        b->sda = sda;
        b->scl = scl;
        if (sda >= 0) {
            esp_gpio_forget((uint8_t)sda);
        }
        if (scl >= 0) {
            esp_gpio_forget((uint8_t)scl);
        }
    }
    b->open = true;
    return ESP_OK;
}

static esp_err_t device(i2c_bus_t *b, uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (addr > 0x7f) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t i = 0; i < b->n_dev; i++) {
        if (b->dev[i].addr == addr) {
            *out = b->dev[i].handle;
            return ESP_OK;
        }
    }
    if (b->n_dev >= I2C_DEVICES) {
        return ESP_ERR_NO_MEM;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = b->speed_hz,
    };
    i2c_master_dev_handle_t h;
    esp_err_t err = i2c_master_bus_add_device(b->bus, &cfg, &h);
    if (err != ESP_OK) {
        return err;
    }
    b->dev[b->n_dev].addr = addr;
    b->dev[b->n_dev].handle = h;
    b->n_dev++;
    *out = h;
    return ESP_OK;
}

esp_err_t esp_i2c_read(uint8_t bus, uint8_t addr, uint8_t reg, bool stop, uint8_t *data, size_t n,
                       uint16_t timeout_ms)
{
    if (bus >= I2C_BUSES || !buses[bus].open || !data || n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t h;
    esp_err_t err = device(&buses[bus], addr, &h);
    if (err != ESP_OK) {
        return fail(err);
    }
    int t = timeout_of(timeout_ms);
    if (!stop) {   /* repeated start between the register write and the read */
        return fail(i2c_master_transmit_receive(h, &reg, 1, data, n, t));
    }
    err = i2c_master_transmit(h, &reg, 1, t);
    return fail(err == ESP_OK ? i2c_master_receive(h, data, n, t) : err);
}

esp_err_t esp_i2c_write(uint8_t bus, uint8_t addr, const uint8_t *data, size_t n, uint16_t timeout_ms)
{
    if (bus >= I2C_BUSES || !buses[bus].open || (n > 0 && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    int t = timeout_of(timeout_ms);
    if (n == 0) {   /* an empty write asks whether the device answers */
        return addr > 0x7f ? ESP_ERR_INVALID_ARG : fail(i2c_master_probe(buses[bus].bus, addr, t));
    }
    i2c_master_dev_handle_t h;
    esp_err_t err = device(&buses[bus], addr, &h);
    return fail(err == ESP_OK ? i2c_master_transmit(h, data, n, t) : err);
}

void esp_i2c_close(uint8_t bus)
{
    if (bus >= I2C_BUSES || !buses[bus].open) {
        return;
    }
    i2c_bus_t *b = &buses[bus];
    for (uint8_t i = 0; i < b->n_dev; i++) {
        i2c_master_bus_rm_device(b->dev[i].handle);
    }
    if (b->owned) {
        i2c_del_master_bus(b->bus);
    }
    if (b->sda >= 0) {
        esp_gpio_forget((uint8_t)b->sda);
    }
    if (b->scl >= 0) {
        esp_gpio_forget((uint8_t)b->scl);
    }
    *b = (i2c_bus_t) {
        0
    };
}
