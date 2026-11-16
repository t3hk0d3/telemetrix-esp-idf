idf_component_register(SRCS "main.c" "host_profile.c"
                       INCLUDE_DIRS "."
                       REQUIRES telemetrix-esp-idf
                       PRIV_INCLUDE_DIRS "../../../src/drivers")
