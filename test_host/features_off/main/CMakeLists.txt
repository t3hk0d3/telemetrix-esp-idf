idf_component_register(SRCS "../../main/test_main.c" "test_features_off.c"
                       REQUIRES telemetrix-esp-idf unity
                       PRIV_INCLUDE_DIRS "../../main" "../../../src/proto" "../../../src/core" "../../../src/adapter"
                                         "../../../src/drivers" "../../../src/net"
                       WHOLE_ARCHIVE)
