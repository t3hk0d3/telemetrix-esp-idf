#ifndef TEST_PLUGIN_H
#define TEST_PLUGIN_H

#include "telemetrix_plugin.h"

/** The test plugin "test.echo" 1.0, declaring the named capability "test.led". Shared by the host unit tests, the
 *  fuzzer and the host server; free of Unity. It drives pins through the fake driver. Sub-commands:
 *  0 [args...]  echo args as PLUGIN_REPORT sub 0
 *  1            refuse with PLUGIN code 7
 *  2 [req u16]  emit_async [req_hi, req_lo, 0x55] from a FreeRTOS task (directly under TMX_FUZZ)
 *  3 [gpio]     claim the pin as test.led and drive it high
 *  4            busy until the driver's micros advanced 50000
 *  5            enable the poll hook: PLUGIN_REPORT sub 9 [] every 1000 ms (release disables it)
 *  The release hook waits (at most 1 s) for pending sub-2 tasks, so none touches the ctx after it returns. */
extern const telemetrix_plugin_t test_plugin_echo;

#define TEST_PLUGIN_REFUSE_CODE 7
#define TEST_PLUGIN_ASYNC_MARK 0x55
#define TEST_PLUGIN_BUSY_US 50000
#define TEST_PLUGIN_TICK_MS 1000
#define TEST_PLUGIN_TICK_SUB 9

/** Forget the poll enable, the claimed pin and the counters. */
void test_plugin_reset(void);
/** Release hook calls since the last reset. */
unsigned test_plugin_releases(void);
/** Async emits that finished since the last reset, and the result of the last one. */
unsigned test_plugin_async_done(void);
esp_err_t test_plugin_async_last(void);
/** The ctx of the last command (NULL after a reset); may be stale once the server stopped. */
telemetrix_plugin_ctx_t *test_plugin_ctx(void);

#endif
