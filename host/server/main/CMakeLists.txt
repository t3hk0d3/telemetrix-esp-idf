idf_component_register(SRCS "main.c" "host_profile.c" "../../../test_host/main/test_plugin.c"
                       INCLUDE_DIRS "." "../../../test_host/main"
                       REQUIRES telemetrix-esp-idf
                       PRIV_INCLUDE_DIRS "../../../src/drivers")
