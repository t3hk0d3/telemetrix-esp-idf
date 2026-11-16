idf_component_register(SRCS "test_main.c" "test_frame.c" "test_stock_cmds.c" "test_reports.c" "test_ext_reports.c" "test_profile.c" "test_claims.c" "test_fake.c" "test_sched.c" "test_adapter_digital.c" "test_adapter_analog.c" "test_adapter_pwm.c" "test_adapter_i2c.c" "test_adapter_onewire.c" "test_adapter_buses.c" "test_adapter_sensors.c" "test_adapter_reasons.c" "test_conn.c" "test_cfg.c" "test_server.c" "test_sock.c" "test_board_poe.c" "test_board_devkit.c" "test_board_devkit_c6.c"
                       REQUIRES telemetrix-esp-idf unity
                       PRIV_INCLUDE_DIRS "../../src/proto" "../../src/core" "../../src/adapter" "../../src/drivers" "../../src/net"
                       WHOLE_ARCHIVE)
