#include "board_check.h"

/* The example profile is a plain table; validate it as telemetrix_start does (the global is renamed so the examples
 * link together). On the linux target CONFIG_IDF_TARGET_* is unset, so this is the S3 table;
 * test_board_devkit_c6.c checks the C6 one. */
#define board_profile devkit_board_profile
#include "../../../examples/devkit_wifi/main/board_profile.c"

TEST_CASE("devkit_wifi profile (S3): passes the profile validation", "[board_profiles]")
{
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&devkit_board_profile));
}

TEST_CASE("devkit_wifi profile (S3): I2C bus 0 has fixed pins that are I2C-capable profile pins", "[board_profiles]")
{
    board_check_i2c_bus0_fixed(&devkit_board_profile);
}
