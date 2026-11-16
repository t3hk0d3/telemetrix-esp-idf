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

bool tmx_adapter_pin_ok(const tmx_adapter_t *a, uint8_t gpio, tmx_use_t use)
{
    const telemetrix_pin_t *pin = tmx_profile_pin(a->profile, gpio);
    uint16_t need = caps_needed(use);
    if (!pin || (pin->caps & need) != need) {
        return false;
    }
    tmx_use_t cur = tmx_pin_use(&a->claims, gpio);
    return cur == TMX_USE_NONE || (cur >= TMX_USE_INPUT && cur <= TMX_USE_SERVO);
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
    clear_tables(a);
    return tmx_claims_init(&a->claims, p, drv);
}

void tmx_adapter_release(tmx_adapter_t *a)
{
    /* Task 7 closes its buses and drops sonar and DHT devices here, before the claims go. */
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
    /* Task 7: drain tmx_completion_take here (sonar echoes, DHT frames) and run the sonar and DHT deadlines. */
    uint32_t next = tmx_deadline_left(&a->digital.scan, now_ms);
    uint32_t analog = tmx_deadline_left(&a->analog.scan, now_ms);
    return analog < next ? analog : next;
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

/* Returns false for a refusal; ignored commands are counted by the caller's verdict. */
static bool set_pin_mode(tmx_adapter_t *a, const uint8_t *p)
{
    uint8_t pin = p[0];
    switch (p[1]) {
    case TMX_MODE_INPUT: return tmx_digital_mode(a, pin, TMX_USE_INPUT, p[2] != 0);
    case TMX_MODE_INPUT_PULLUP: return tmx_digital_mode(a, pin, TMX_USE_INPUT_PULLUP, p[2] != 0);
    case TMX_MODE_INPUT_PULLDOWN: return tmx_digital_mode(a, pin, TMX_USE_INPUT_PULLDOWN, p[2] != 0);
    case TMX_MODE_OUTPUT: return tmx_digital_mode(a, pin, TMX_USE_OUTPUT, false);
    case TMX_MODE_ANALOG: return tmx_analog_mode(a, pin, (uint16_t)(p[2] << 8 | p[3]), p[4] != 0);
    case TMX_MODE_PWM: return tmx_pwm_mode(a, pin, p[3], decode_double_le(p + 4));
    default: return false;
    }
}

static bool modify_reporting(tmx_adapter_t *a, uint8_t mode, uint8_t pin)
{
    switch (mode) {
    case 0: {
        bool digital = tmx_digital_set_report(a, pin, false);
        return tmx_analog_set_report(a, pin, false) || digital;
    }
    case 1: return tmx_analog_set_report(a, pin, true);
    case 2: return tmx_digital_set_report(a, pin, true);
    case 3: return tmx_analog_set_report(a, pin, false);
    case 4: return tmx_digital_set_report(a, pin, false);
    default: return false;
    }
}

static bool servo_command(tmx_adapter_t *a, uint8_t id, const uint8_t *p)
{
    uint8_t out[TMX_REPORT_MAX];
    switch (id) {
    case TMX_CMD_SERVO_ATTACH: {
        tmx_res_t res = tmx_servo_attach(a, p[0], (uint16_t)(p[1] << 8 | p[2]), (uint16_t)(p[3] << 8 | p[4]));
        if (res == TMX_RES_UNAVAILABLE) {
            tmx_adapter_emit(a, out, tmx_rpt_servo_unavailable(out, p[0]), true, TMX_COALESCE_NONE, 0);
        }
        return res != TMX_RES_REFUSED;
    }
    case TMX_CMD_SERVO_WRITE: return tmx_servo_write(a, p[0], p[1]);
    default: return tmx_servo_detach(a, p[0]);
    }
}

void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms)
{
    switch (tmx_stock_check(id, payload, len)) {
    case TMX_STOCK_SHORT:
        a->refused_commands++;
        return;
    case TMX_STOCK_IGNORED:
        a->ignored_commands++;
        return;
    default:
        break;
    }
    const uint8_t *p = payload;
    uint8_t out[TMX_REPORT_MAX];
    bool ok = true;
    switch (id) {
    case TMX_CMD_LOOP_COMMAND:
        tmx_adapter_emit(a, out, tmx_rpt_loop(out, p[0]), true, TMX_COALESCE_NONE, 0);
        break;
    case TMX_CMD_SET_PIN_MODE:
        if (p[1] == TMX_MODE_ANALOG && !ANALOG_ENABLED) {
            a->ignored_commands++;
            return;
        }
        ok = known_mode(p[1]) && set_pin_mode(a, p);
        break;
    case TMX_CMD_DIGITAL_WRITE:
        ok = tmx_digital_write(a, p[0], p[1] != 0);
        break;
    case TMX_CMD_ANALOG_WRITE:
        ok = tmx_pwm_write(a, p[0], (uint16_t)(p[1] << 8 | p[2]));
        break;
    case TMX_CMD_MODIFY_REPORTING:
        ok = modify_reporting(a, p[0], p[1]);
        break;
    case TMX_CMD_GET_FIRMWARE_VERSION:
        tmx_adapter_emit(a, out, tmx_rpt_firmware(out), true, TMX_COALESCE_NONE, 0);
        break;
    case TMX_CMD_SERVO_ATTACH:
    case TMX_CMD_SERVO_WRITE:
    case TMX_CMD_SERVO_DETACH:
        if (!SERVO_ENABLED) {
            a->ignored_commands++;
            return;
        }
        ok = servo_command(a, id, p);
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
        ok = tmx_pwm_attach(a, p[0]);
        break;
    case TMX_CMD_ANALOG_OUT_DETACH:
        ok = tmx_pwm_detach(a, p[0]);
        break;
    case TMX_CMD_RESET:
        tmx_adapter_release(a);
        a->reset_requested = true;
        break;
    default:
        /* Task 7: I2C, sonar, DHT and 1-wire commands. */
        break;
    }
    if (!ok) {
        a->refused_commands++;
    }
    schedule(a, now_ms);
}
