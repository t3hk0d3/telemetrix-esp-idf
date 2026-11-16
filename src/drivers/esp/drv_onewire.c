#include "esp_log.h"
#include "onewire_bus.h"
#include "onewire_device.h"

#include "tmx_drv_esp.h"

static const char *TAG = "telemetrix";

#define OW_BUSES 2
#define OW_MAX_RX_BYTES 10   /* a ROM id plus command margin per read call */

typedef struct {
    onewire_bus_handle_t bus;   /* NULL = closed */
    onewire_device_iter_handle_t iter;
    uint8_t gpio;
} ow_t;

static ow_t buses[OW_BUSES];
static uint8_t tx_granted;
static bool crc_logged;

void esp_onewire_init(const telemetrix_profile_t *p)
{
    tx_granted = p->rmt.tx_channels;
    crc_logged = false;
}

static void drop_iter(ow_t *o)
{
    if (o->iter) {
        onewire_del_device_iter(o->iter);
        o->iter = NULL;
    }
}

esp_err_t esp_ow_open(uint8_t bus, uint8_t gpio)
{
    if (bus >= OW_BUSES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buses[bus].bus) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t open = 0;
    for (int i = 0; i < OW_BUSES; i++) {
        open += buses[i].bus != NULL;
    }
    if (open >= tx_granted) {   /* the bus takes a TX channel next to the RX channel the adapter claimed */
        return ESP_ERR_NOT_FOUND;
    }
    const onewire_bus_config_t cfg = {.bus_gpio_num = gpio, .flags.en_pull_up = true};
    const onewire_bus_rmt_config_t rmt = {.max_rx_bytes = OW_MAX_RX_BYTES};
    onewire_bus_handle_t h = NULL;
    esp_gpio_forget(gpio);
    esp_err_t err = onewire_new_bus_rmt(&cfg, &rmt, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "1-wire bus %u on gpio %u: %s", bus, gpio, esp_err_to_name(err));
        return err;
    }
    buses[bus] = (ow_t) {
        .bus = h, .gpio = gpio
    };
    return ESP_OK;
}

bool esp_ow_reset(uint8_t bus)
{
    return bus < OW_BUSES && buses[bus].bus && onewire_bus_reset(buses[bus].bus) == ESP_OK;
}

/* onewire_bus has no strong pull-up: `power` is ignored, a parasite-powered device needs an external pull-up. */
void esp_ow_write(uint8_t bus, uint8_t byte, bool power)
{
    (void)power;
    if (bus < OW_BUSES && buses[bus].bus) {
        onewire_bus_write_bytes(buses[bus].bus, &byte, 1);
    }
}

uint8_t esp_ow_read(uint8_t bus)
{
    uint8_t b = 0xff;
    if (bus < OW_BUSES && buses[bus].bus && onewire_bus_read_bytes(buses[bus].bus, &b, 1) != ESP_OK) {
        return 0xff;
    }
    return b;
}

void esp_ow_reset_search(uint8_t bus)
{
    if (bus < OW_BUSES) {
        drop_iter(&buses[bus]);
    }
}

/* The ROM id goes out least significant byte first: family code, serial, CRC. The component checks the CRC. */
bool esp_ow_search(uint8_t bus, uint8_t rom[8])
{
    if (bus >= OW_BUSES || !buses[bus].bus) {
        return false;
    }
    ow_t *o = &buses[bus];
    if (!o->iter && onewire_new_device_iter(o->bus, &o->iter) != ESP_OK) {
        o->iter = NULL;
        return false;
    }
    onewire_device_t dev;
    esp_err_t err = onewire_device_iter_get_next(o->iter, &dev);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_CRC && !crc_logged) {   /* counts as no device */
            crc_logged = true;
            ESP_LOGW(TAG, "1-wire search on gpio %u: bad CRC in a ROM id", o->gpio);
        }
        return false;
    }
    for (int i = 0; i < 8; i++) {
        rom[i] = (uint8_t)(dev.address >> (8 * i));
    }
    return true;
}

void esp_ow_close(uint8_t bus)
{
    if (bus >= OW_BUSES || !buses[bus].bus) {
        return;
    }
    drop_iter(&buses[bus]);
    onewire_bus_del(buses[bus].bus);
    esp_gpio_forget(buses[bus].gpio);
    buses[bus] = (ow_t) {
        0
    };
}

void esp_onewire_deinit(void)
{
    for (uint8_t i = 0; i < OW_BUSES; i++) {
        esp_ow_close(i);
    }
}
