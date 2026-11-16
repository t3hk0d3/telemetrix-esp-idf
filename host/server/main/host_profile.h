#ifndef HOST_PROFILE_H
#define HOST_PROFILE_H

#include <stdbool.h>

#include "telemetrix.h"

/** The profile of a classic ESP32 as far as the stock client sees it, plus a second I2C bus (sda 2, scl 15), a second
 *  1-wire bus (gpio 35) and gpio 36. test_plugin: gpio 36 carries the named capability "test.led" (test.echo must be
 *  registered then). */
const telemetrix_profile_t *host_profile(bool test_plugin);
/** Script the fake driver: loopbacks, ADC, I2C devices (0x76 on bus 0, 0x48 on bus 1), DHT, sonar and a 1-wire ROM on
 *  each bus. */
void host_script_fake(void);

#endif
