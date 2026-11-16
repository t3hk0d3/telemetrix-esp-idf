/* Stand-in for the generated sdkconfig.h: esp_err.h (via esp_compiler.h) includes it, and the core and the adapter
 * read these switches. The fuzz build counts as the linux target and has every optional feature on. */
#define CONFIG_IDF_TARGET_LINUX 1
#define CONFIG_TELEMETRIX_ANALOG 1
#define CONFIG_TELEMETRIX_SERVO 1
#define CONFIG_TELEMETRIX_I2C 1
#define CONFIG_TELEMETRIX_SONAR 1
#define CONFIG_TELEMETRIX_DHT 1
#define CONFIG_TELEMETRIX_ONE_WIRE 1
