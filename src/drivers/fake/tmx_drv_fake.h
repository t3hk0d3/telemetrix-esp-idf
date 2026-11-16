#ifndef TMX_DRV_FAKE_H
#define TMX_DRV_FAKE_H

#include "tmx_drv.h"

/** The scriptable fake driver: records every call and answers from the values a test sets. */
extern const tmx_drv_t tmx_drv_fake;

/** Forget every scripted value and every recorded call. */
void tmx_fake_reset(void);
/** The level an input without a loopback reads (default 0). */
void tmx_fake_set_level(uint8_t gpio, bool v);
/** Wire out_gpio to in_gpio: while out_gpio is an output, in_gpio reads its level; otherwise in_gpio reads its pull. */
void tmx_fake_loopback(uint8_t out_gpio, uint8_t in_gpio);
void tmx_fake_set_adc(uint8_t unit, uint8_t channel, int raw);
/** An I2C device at addr on bus whose register map is regs[0..n) (at most 256 bytes). */
void tmx_fake_i2c_device(uint8_t bus, uint8_t addr, const uint8_t *regs, size_t n);
/** What dht_start on gpio will deliver: the five frame bytes with err ESP_OK, or err (e.g. ESP_ERR_TIMEOUT). */
void tmx_fake_dht_frame(uint8_t gpio, const uint8_t f[5], esp_err_t err);
/** What sonar_start on trig will deliver, in microseconds; 0 (the default) delivers ESP_ERR_TIMEOUT. */
void tmx_fake_sonar_echo(uint8_t trig, uint32_t us);
/** dht_start and sonar_start only remember the call and its token. This posts every pending start, in start order,
 *  through tmx_completion_post with the scripted result and forgets them. Without it nothing ever completes. */
void tmx_fake_complete_pending(void);
/** The ROMs the 1-wire search on bus finds. */
void tmx_fake_ow_devices(uint8_t bus, const uint8_t (*roms)[8], size_t n);

/** The next call of op (a driver function name, e.g. "ledc_timer") is recorded, does nothing else and returns err once.
 *  Works for every operation returning esp_err_t; cleared by tmx_fake_reset. */
void tmx_fake_fail_next(const char *op, esp_err_t err);
/** RMT RX channels the driver holds now: the shared DHT channel (from the first dht_start until release or deinit)
 *  and one per open 1-wire bus. */
size_t tmx_fake_rmt_rx_live(void);
/** True while the echo handler of a sonar_start is attached (until release or deinit). */
bool tmx_fake_sonar_handler(void);

/** True once a call was left out of the log because it was full (cleared by tmx_fake_reset). */
bool tmx_fake_log_truncated(void);

/** The recorded calls as "name(args)" lines, each ending in a newline. gpio_get is not recorded. */
const char *tmx_fake_calls(void);
/** How many recorded calls are named name. */
size_t tmx_fake_count(const char *name);

#endif
