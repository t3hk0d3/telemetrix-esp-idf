#ifndef NETWORK_H
#define NETWORK_H

#include "esp_err.h"

/** Bring the network up and return once the interface has an IPv4 address. Logs while it waits. */
esp_err_t example_network_start(void);

#endif
