#ifndef HOST_PROFILE_H
#define HOST_PROFILE_H

#include "telemetrix.h"

/** The profile of a classic ESP32 as far as the stock client sees it, and the fake devices it finds. */
const telemetrix_profile_t *host_profile(void);
/** Script the fake driver: loopbacks, ADC, I2C device, DHT, sonar and the 1-wire ROM. */
void host_script_fake(void);

#endif
