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
/** What dht_start on gpio will deliver (completion posting arrives with the completion queue). */
void tmx_fake_dht_frame(uint8_t gpio, const uint8_t f[5], esp_err_t err);
/** What sonar_start on trig will deliver, in microseconds. */
void tmx_fake_sonar_echo(uint8_t trig, uint32_t us);
/** The ROMs the 1-wire search on bus finds. */
void tmx_fake_ow_devices(uint8_t bus, const uint8_t (*roms)[8], size_t n);

/** The recorded calls as "name(args)" lines, each ending in a newline. gpio_get is not recorded. */
const char *tmx_fake_calls(void);
/** How many recorded calls are named name. */
size_t tmx_fake_count(const char *name);

#endif
