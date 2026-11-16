idf_component_register(SRCS "test_main.c" "test_frame.c" "test_stock_cmds.c" "test_reports.c" "test_profile.c" "test_claims.c" "test_fake.c" "test_sched.c" "test_adapter_digital.c" "test_adapter_analog.c" "test_adapter_pwm.c"
                       REQUIRES telemetrix-esp-idf unity
                       PRIV_INCLUDE_DIRS "../../src/proto" "../../src/core" "../../src/adapter" "../../src/drivers"
                       WHOLE_ARCHIVE)
