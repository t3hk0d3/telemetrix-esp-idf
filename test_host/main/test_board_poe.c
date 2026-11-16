#include "board_check.h"

/* The example profile is a plain table; validate it as telemetrix_start does (the global is renamed so the examples
 * link together). */
#define board_profile poe_board_profile
#include "../../../examples/esp32_poe/main/board_profile.c"

TEST_CASE("esp32_poe profile: passes the profile validation", "[board_profiles]")
{
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&poe_board_profile));
}

TEST_CASE("esp32_poe profile: I2C bus 0 has fixed pins that are I2C-capable profile pins", "[board_profiles]")
{
    board_check_i2c_bus0_fixed(&poe_board_profile);
}

TEST_CASE("esp32_poe profile: I2C bus 0 is the UEXT pair, SDA GPIO13 and SCL GPIO16", "[board_profiles]")
{
    TEST_ASSERT_EQUAL(13, poe_board_profile.i2c[0].sda);
    TEST_ASSERT_EQUAL(16, poe_board_profile.i2c[0].scl);
}
