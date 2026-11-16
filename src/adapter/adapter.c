#include <string.h>

#include "sdkconfig.h"
#include "tmx_adapter.h"

#ifdef CONFIG_TELEMETRIX_ANALOG
#define ANALOG_ENABLED true
#else
#define ANALOG_ENABLED false
#endif
#ifdef CONFIG_TELEMETRIX_SERVO
#define SERVO_ENABLED true
#else
#define SERVO_ENABLED false
#endif
#ifdef CONFIG_TELEMETRIX_I2C
#define I2C_ENABLED true
#else
#define I2C_ENABLED false
#endif
#ifdef CONFIG_TELEMETRIX_SONAR
#define SONAR_ENABLED true
#else
#define SONAR_ENABLED false
#endif
#ifdef CONFIG_TELEMETRIX_DHT
#define DHT_ENABLED true
#else
#define DHT_ENABLED false
#endif
#ifdef CONFIG_TELEMETRIX_ONE_WIRE
#define ONE_WIRE_ENABLED true
#else
#define ONE_WIRE_ENABLED false
#endif

int tmx_adapter_pin_index(const tmx_adapter_t *a, uint8_t gpio)
{
    for (size_t i = 0; i < a->profile->n_pins; i++) {
        if (a->profile->pins[i].gpio == gpio) {
            return (int)i;
        }
    }
    return -1;
}

static uint16_t caps_needed(tmx_use_t use)
{
    switch (use) {
    case TMX_USE_INPUT: return TELEMETRIX_CAP_DIN;
    case TMX_USE_INPUT_PULLUP: return TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_PULL_UP;
    case TMX_USE_INPUT_PULLDOWN: return TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_PULL_DOWN;
    case TMX_USE_OUTPUT: return TELEMETRIX_CAP_DOUT;
    case TMX_USE_ANALOG: return TELEMETRIX_CAP_ADC;
    case TMX_USE_PWM:
    case TMX_USE_SERVO: return TELEMETRIX_CAP_PWM;
    default: return 0xffff;
    }
}

static bool is_pin_mode(tmx_use_t use)
{
    return use >= TMX_USE_INPUT && use <= TMX_USE_SERVO;
}

telemetrix_reason_t tmx_adapter_pin_check(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use)
{
    const telemetrix_pin_t *pin = tmx_profile_pin(a->profile, gpio);
    uint16_t need = caps_needed(use);
    if (!pin) {
        return TELEMETRIX_REASON_NO_SUCH_PIN;
    }
    if ((pin->caps & need) != need) {
        return TELEMETRIX_REASON_NO_CAPABILITY;
    }
    tmx_use_t cur = tmx_pin_use(&a->claims, gpio);
    return cur == TMX_USE_NONE || is_pin_mode(cur) ? TELEMETRIX_REASON_NONE : TELEMETRIX_REASON_PIN_BUSY;
}

telemetrix_reason_t tmx_adapter_pin_free(const tmx_adapter_t *a, int gpio, uint16_t need)
{
    const telemetrix_pin_t *pin = gpio < 0 || gpio > UINT8_MAX ? NULL : tmx_profile_pin(a->profile, (uint8_t)gpio);
    if (!pin) {
        return TELEMETRIX_REASON_NO_SUCH_PIN;
    }
    if ((pin->caps & need) != need) {
        return TELEMETRIX_REASON_NO_CAPABILITY;
    }
    return tmx_pin_use(&a->claims, (uint8_t)gpio) == TMX_USE_NONE ? TELEMETRIX_REASON_NONE : TELEMETRIX_REASON_PIN_BUSY;
}

telemetrix_reason_t tmx_adapter_pin_state(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use)
{
    tmx_use_t cur = tmx_pin_use(&a->claims, gpio);
    if (cur == use) {
        return TELEMETRIX_REASON_NONE;
    }
    if (!tmx_profile_pin(a->profile, gpio)) {
        return TELEMETRIX_REASON_NO_SUCH_PIN;
    }
    return cur == TMX_USE_NONE || is_pin_mode(cur) ? TELEMETRIX_REASON_BAD_ARGUMENT : TELEMETRIX_REASON_PIN_BUSY;
}

telemetrix_reason_t tmx_adapter_claim_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_SUPPORTED: return TELEMETRIX_REASON_NO_CAPABILITY;
    case ESP_ERR_INVALID_STATE: return TELEMETRIX_REASON_PIN_BUSY;
    default: return TELEMETRIX_REASON_HARDWARE;
    }
}

void tmx_adapter_retire(tmx_adapter_t *a, uint8_t gpio)
{
    int idx = tmx_adapter_pin_index(a, gpio);
    if (idx < 0) {
        return;
    }
    tmx_pwm_forget(a, idx);     /* stops the LEDC channel before the pin goes to its safe state */
    tmx_digital_forget(a, idx);
    tmx_analog_forget(a, idx);
    tmx_release_pin(&a->claims, gpio);
}

void tmx_adapter_emit(tmx_adapter_t *a, const uint8_t *report, size_t n, bool reply, int8_t kind, uint16_t key)
{
    uint8_t wrapped[TMX_REPORT_MAX];
    if (a->on_bus >= 0) {
        size_t m = tmx_rpt_bus_wrap(wrapped, (uint8_t)a->on_bus, report, n);
        if (m > 0) {
            a->emit(wrapped, m, reply, kind, key, a->ctx);
        }
        return;
    }
    a->emit(report, n, reply, kind, key, a->ctx);
}

static void clear_tables(tmx_adapter_t *a)
{
    memset(&a->digital, 0, sizeof(a->digital));
    memset(&a->analog, 0, sizeof(a->analog));
    memset(&a->pwm, 0, sizeof(a->pwm));
    a->analog.interval_ms = tmx_analog_default_interval(a);
    a->reporting = true;
}

esp_err_t tmx_adapter_init(tmx_adapter_t *a, const telemetrix_profile_t *p, const telemetrix_config_t *cfg,
                           const tmx_drv_t *drv, tmx_emit_fn emit, void *ctx)
{
    if (!a || !p || !cfg || !drv || !emit) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(a, 0, sizeof(*a));
    a->profile = p;
    a->cfg = *cfg;
    a->drv = drv;
    a->emit = emit;
    a->ctx = ctx;
    a->on_bus = -1;
    clear_tables(a);
    return tmx_claims_init(&a->claims, p, drv);
}

void tmx_adapter_release(tmx_adapter_t *a)
{
    tmx_i2c_release(a);         /* buses closed and devices dropped before their pins and RMT claims go */
    tmx_onewire_release(a);
    tmx_sensors_release(a);
    a->drv->release();          /* the DHT channel and the echo handler, so the claims match what the driver holds */
    tmx_claims_release_all(&a->claims);
    clear_tables(a);
}

void tmx_adapter_counters(const tmx_adapter_t *a, uint32_t *ignored, uint32_t *refused)
{
    if (ignored) {
        *ignored = a->ignored_commands;
    }
    if (refused) {
        *refused = a->refused_commands;
    }
}

size_t tmx_adapter_pin_uses(const tmx_adapter_t *a, telemetrix_pin_use_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < a->profile->n_pins && n < cap; i++) {
        tmx_use_t use = (tmx_use_t)a->claims.use[i];
        if (use != TMX_USE_NONE) {
            out[n].gpio = a->profile->pins[i].gpio;
            out[n].use = tmx_use_name(use);
            n++;
        }
    }
    return n;
}

bool tmx_adapter_reset_requested(tmx_adapter_t *a)
{
    bool r = a->reset_requested;
    a->reset_requested = false;
    return r;
}

static void schedule(tmx_adapter_t *a, uint32_t now_ms)
{
    bool digital = a->reporting && tmx_digital_wanted(a);
    bool analog = a->reporting && tmx_analog_wanted(a);
    if (!digital) {
        a->digital.scan.armed = false;
    } else if (!a->digital.scan.armed) {
        tmx_deadline_arm(&a->digital.scan, now_ms, TMX_DIGITAL_POLL_MS);
    }
    if (!analog) {
        a->analog.scan.armed = false;
    } else if (!a->analog.scan.armed) {
        tmx_deadline_arm(&a->analog.scan, now_ms, a->analog.interval_ms);
    }
}

uint32_t tmx_adapter_poll(tmx_adapter_t *a, uint32_t now_ms)
{
    schedule(a, now_ms);
    if (tmx_deadline_due(&a->digital.scan, now_ms)) {
        tmx_digital_scan(a);
        tmx_deadline_arm(&a->digital.scan, now_ms, TMX_DIGITAL_POLL_MS);
    }
    if (tmx_deadline_due(&a->analog.scan, now_ms)) {
        tmx_analog_scan(a);
        tmx_deadline_arm(&a->analog.scan, now_ms, a->analog.interval_ms);
    }
    tmx_sensors_poll(a, now_ms);
    uint32_t next = tmx_deadline_left(&a->digital.scan, now_ms);
    uint32_t analog = tmx_deadline_left(&a->analog.scan, now_ms);
    uint32_t sensors = tmx_sensors_left(a, now_ms);
    next = analog < next ? analog : next;
    return sensors < next ? sensors : next;
}

static bool known_mode(uint8_t mode)
{
    return mode == TMX_MODE_INPUT || mode == TMX_MODE_OUTPUT || mode == TMX_MODE_INPUT_PULLUP ||
           mode == TMX_MODE_ANALOG || mode == TMX_MODE_PWM || mode == TMX_MODE_INPUT_PULLDOWN;
}

static double decode_double_le(const uint8_t *p)
{
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) {
        bits |= (uint64_t)p[i] << (8 * i);
    }
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static telemetrix_reason_t set_pin_mode(tmx_adapter_t *a, const uint8_t *p)
{
    uint8_t pin = p[0];
    switch (p[1]) {
    case TMX_MODE_INPUT: return tmx_digital_mode(a, pin, TMX_USE_INPUT, p[2] != 0);
    case TMX_MODE_INPUT_PULLUP: return tmx_digital_mode(a, pin, TMX_USE_INPUT_PULLUP, p[2] != 0);
    case TMX_MODE_INPUT_PULLDOWN: return tmx_digital_mode(a, pin, TMX_USE_INPUT_PULLDOWN, p[2] != 0);
    case TMX_MODE_OUTPUT: return tmx_digital_mode(a, pin, TMX_USE_OUTPUT, false);
    case TMX_MODE_ANALOG: return tmx_analog_mode(a, pin, (uint16_t)(p[2] << 8 | p[3]), p[4] != 0);
    case TMX_MODE_PWM: return tmx_pwm_mode(a, pin, p[3], decode_double_le(p + 4));
    default: return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
}

static telemetrix_reason_t modify_reporting(tmx_adapter_t *a, uint8_t mode, uint8_t pin)
{
    switch (mode) {
    case 0:     /* upstream: every pin's reporting off, the pin byte ignored, the global flag untouched */
        memset(a->digital.report, 0, sizeof(a->digital.report));
        memset(a->analog.report, 0, sizeof(a->analog.report));
        return TELEMETRIX_REASON_NONE;
    case 1: return tmx_analog_set_report(a, pin, true);
    case 2: return tmx_digital_set_report(a, pin, true);
    case 3: return tmx_analog_set_report(a, pin, false);
    case 4: return tmx_digital_set_report(a, pin, false);
    default: return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
}

static telemetrix_reason_t servo_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p)
{
    switch (id) {
    case TMX_CMD_SERVO_ATTACH:
        return tmx_servo_attach(a, p[0], (uint16_t)(p[1] << 8 | p[2]), (uint16_t)(p[3] << 8 | p[4]));
    case TMX_CMD_SERVO_WRITE: return tmx_servo_write(a, p[0], p[1]);
    default: return tmx_servo_detach(a, p[0]);
    }
}

/** A command of a feature switched off in Kconfig: ignored and counted like an id out of scope. */
static bool switched_off(uint8_t id, const uint8_t *p)
{
    switch (id) {
    case TMX_CMD_SET_PIN_MODE: return p[1] == TMX_MODE_ANALOG && !ANALOG_ENABLED;
    case TMX_CMD_SERVO_ATTACH:
    case TMX_CMD_SERVO_WRITE:
    case TMX_CMD_SERVO_DETACH: return !SERVO_ENABLED;
    case TMX_CMD_I2C_BEGIN:
    case TMX_CMD_I2C_READ:
    case TMX_CMD_I2C_WRITE: return !I2C_ENABLED;
    case TMX_CMD_SONAR_NEW: return !SONAR_ENABLED;
    case TMX_CMD_DHT_NEW: return !DHT_ENABLED;
    default: return id >= TMX_CMD_ONE_WIRE_INIT && id <= TMX_CMD_ONE_WIRE_CRC8 && !ONE_WIRE_ENABLED;
    }
}

telemetrix_reason_t tmx_adapter_command(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len,
                                        uint32_t now_ms)
{
    switch (tmx_stock_check(id, payload, len)) {
    case TMX_STOCK_SHORT:
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    case TMX_STOCK_IGNORED:
        return TELEMETRIX_REASON_NOT_IN_SCOPE;
    default:
        break;
    }
    if (switched_off(id, payload)) {
        return TELEMETRIX_REASON_NOT_IN_SCOPE;
    }
    const uint8_t *p = payload;
    uint8_t out[TMX_REPORT_MAX];
    telemetrix_reason_t why = TELEMETRIX_REASON_NONE;
    switch (id) {
    case TMX_CMD_LOOP_COMMAND:
        tmx_adapter_emit(a, out, tmx_rpt_loop(out, p[0]), true, TMX_COALESCE_NONE, 0);
        break;
    case TMX_CMD_SET_PIN_MODE:
        why = known_mode(p[1]) ? set_pin_mode(a, p) : TELEMETRIX_REASON_BAD_ARGUMENT;
        break;
    case TMX_CMD_DIGITAL_WRITE:
        why = tmx_digital_write(a, p[0], p[1] != 0);
        break;
    case TMX_CMD_ANALOG_WRITE:
        why = tmx_pwm_write(a, p[0], (uint16_t)(p[1] << 8 | p[2]));
        break;
    case TMX_CMD_MODIFY_REPORTING:
        why = modify_reporting(a, p[0], p[1]);
        break;
    case TMX_CMD_GET_FIRMWARE_VERSION:
        tmx_adapter_emit(a, out, tmx_rpt_firmware(out), true, TMX_COALESCE_NONE, 0);
        break;
    case TMX_CMD_SERVO_ATTACH:
    case TMX_CMD_SERVO_WRITE:
    case TMX_CMD_SERVO_DETACH:
        why = servo_command(a, id, p);
        break;
    case TMX_CMD_STOP_ALL_REPORTS:
        a->reporting = false;
        break;
    case TMX_CMD_ENABLE_ALL_REPORTS:
        a->reporting = true;
        break;
    case TMX_CMD_SET_ANALOG_SCANNING_INTERVAL:
        tmx_analog_set_interval(a, p[0]);
        break;
    case TMX_CMD_ANALOG_OUT_ATTACH:
        why = tmx_pwm_attach(a, p[0]);
        break;
    case TMX_CMD_ANALOG_OUT_DETACH:
        why = tmx_pwm_detach(a, p[0]);
        break;
    case TMX_CMD_RESET:
        tmx_adapter_release(a);
        a->reset_requested = true;
        break;
    case TMX_CMD_I2C_BEGIN:
    case TMX_CMD_I2C_READ:
    case TMX_CMD_I2C_WRITE:
        why = tmx_i2c_command(a, 0, id, p, len);
        break;
    case TMX_CMD_SONAR_NEW:
        why = tmx_sonar_new(a, p[0], p[1], now_ms);
        break;
    case TMX_CMD_DHT_NEW:
        why = tmx_dht_new(a, p[0], now_ms);
        break;
    default:
        if (id >= TMX_CMD_ONE_WIRE_INIT && id <= TMX_CMD_ONE_WIRE_CRC8) {
            why = tmx_onewire_command(a, 0, id, p, len);
        }
        break;
    }
    schedule(a, now_ms);
    return why;
}

void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms)
{
    telemetrix_reason_t why = tmx_adapter_command(a, id, payload, len, now_ms);
    uint8_t out[TMX_REPORT_MAX];
    if (why == TELEMETRIX_REASON_NONE) {
        return;
    }
    if (why == TELEMETRIX_REASON_NOT_IN_SCOPE) {
        a->ignored_commands++;
    } else if (why == TELEMETRIX_REASON_EXHAUSTED && id == TMX_CMD_SERVO_ATTACH) {
        /* upstream's report; the command is not counted as refused */
        tmx_adapter_emit(a, out, tmx_rpt_servo_unavailable(out, payload[0]), true, TMX_COALESCE_NONE, 0);
    } else if (why == TELEMETRIX_REASON_NO_ANSWER && id == TMX_CMD_I2C_READ) {
        tmx_adapter_emit(a, out, tmx_rpt_i2c_short(out, payload[0]), true, TMX_COALESCE_NONE, 0);
    } else {
        a->refused_commands++;
    }
}
