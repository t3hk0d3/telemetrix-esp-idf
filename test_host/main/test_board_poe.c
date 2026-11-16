#include "tmx_core.h"
#include "unity.h"

/* The example profile is a plain table; validate it as telemetrix_start does (the global is renamed so the two
 * examples link together). On the linux target CONFIG_IDF_TARGET_*
 * is unset, so devkit_wifi contributes its S3 table. */
#define board_profile poe_board_profile
#include "../../../examples/esp32_poe/main/board_profile.c"

TEST_CASE("esp32_poe profile: passes the profile validation", "[board_profiles]")
{
    TEST_ASSERT_EQUAL(ESP_OK, tmx_profile_validate(&poe_board_profile));
}
