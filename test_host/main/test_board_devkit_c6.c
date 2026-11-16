#include "board_check.h"

/* The C6 table of the devkit_wifi example: the linux sdkconfig defines no CONFIG_IDF_TARGET_ESP32C6, so it is set here
 * before the table is included. */
#define CONFIG_IDF_TARGET_ESP32C6 1
#define board_profile devkit_c6_board_profile
#include "../../../examples/devkit_wifi/main/board_profile.c"

TEST_CASE("devkit_wifi profile (C6): passes the profile validation", "[board_profiles]")
{
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&devkit_c6_board_profile));
}

TEST_CASE("devkit_wifi profile (C6): I2C bus 0 has fixed pins that are I2C-capable profile pins", "[board_profiles]")
{
    board_check_i2c_bus0_fixed(&devkit_c6_board_profile);
}
