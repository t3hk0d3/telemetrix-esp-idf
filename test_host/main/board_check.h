#ifndef BOARD_CHECK_H
#define BOARD_CHECK_H

#include "tmx_core.h"
#include "unity.h"

/** The stock client's I2C_BEGIN names no pins, so bus 0 of an example profile must have fixed pins, and they must be
 *  profile pins with the I2C capability. */
static inline void board_check_i2c_bus0_fixed(const telemetrix_profile_t *p)
{
    const telemetrix_i2c_bus_t *bus0 = NULL;
    for (size_t i = 0; i < p->n_i2c; i++) {
        if (p->i2c[i].bus == 0) {
            bus0 = &p->i2c[i];
        }
    }
    TEST_ASSERT_NOT_NULL(bus0);
    TEST_ASSERT_TRUE(bus0->sda >= 0);
    TEST_ASSERT_TRUE(bus0->scl >= 0);
    TEST_ASSERT_NOT_EQUAL(bus0->sda, bus0->scl);
    const telemetrix_pin_t *sda = tmx_profile_pin(p, (uint8_t)bus0->sda);
    const telemetrix_pin_t *scl = tmx_profile_pin(p, (uint8_t)bus0->scl);
    TEST_ASSERT_NOT_NULL(sda);
    TEST_ASSERT_NOT_NULL(scl);
    TEST_ASSERT_TRUE(sda->caps & TELEMETRIX_CAP_I2C);
    TEST_ASSERT_TRUE(scl->caps & TELEMETRIX_CAP_I2C);
}

#endif
