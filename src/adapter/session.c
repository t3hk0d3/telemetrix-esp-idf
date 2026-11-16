#include <string.h>

#include "sdkconfig.h"
#include "tmx_adapter.h"

#ifdef CONFIG_TELEMETRIX_EXTENDED
#define EXTENDED_ENABLED true
#else
#define EXTENDED_ENABLED false
#endif

static void session_reset(tmx_adapter_t *a)
{
    memset(&a->session, 0, sizeof(a->session));
    a->session.abort = true;
}

void tmx_session_init(tmx_adapter_t *a)
{
    session_reset(a);
}

void tmx_adapter_session_end(tmx_adapter_t *a)
{
    uint32_t errors = a->session.error_reports;     /* a lifetime counter: it survives the session */
    tmx_adapter_release(a);
    session_reset(a);
    a->session.error_reports = errors;
}

bool tmx_adapter_extended(const tmx_adapter_t *a)
{
    return a->session.extended;
}

bool tmx_adapter_violation(const tmx_adapter_t *a)
{
    return a->session.violation;
}

bool tmx_adapter_mode_changed(tmx_adapter_t *a)
{
    bool changed = a->session.mode_changed;
    a->session.mode_changed = false;
    return changed;
}

static void emit_long_answer(tmx_adapter_t *a)
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n = tmx_rpt_firmware_long(out, TMX_EXT_MAJOR, TMX_EXT_MINOR);
    a->emit(out, n, true, TMX_COALESCE_NONE, 0, a->ctx);
}

/** An ERROR report; a PLUGIN command whose payload has [slot, sub] names them in the report, and the plugin's own code
 *  follows when it refused with PLUGIN. */
static void emit_error(tmx_adapter_t *a, uint16_t seq, uint8_t cmd, telemetrix_reason_t why, const uint8_t *payload,
                       size_t len, uint8_t code)
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    const bool named = cmd == TMX_CMD_PLUGIN && len >= 2;
    const bool coded = named && why == TELEMETRIX_REASON_PLUGIN;
    size_t n = tmx_rpt_error(out, seq, cmd, (uint8_t)why, named ? payload[0] : -1, named ? payload[1] : 0,
                             coded ? code : -1);
    a->emit(out, n, true, TMX_COALESCE_NONE, 0, a->ctx);
    a->session.error_reports++;
}

/** The handshake rule of a stock-mode GET_FIRMWARE_VERSION; false when the packet is an ordinary one. */
static bool handshake(tmx_adapter_t *a, const uint8_t *payload, size_t len)
{
    if (!EXTENDED_ENABLED || !a->cfg.extended || len != 3 || payload[0] != TMX_EXT_MAGIC) {
        return false;
    }
    emit_long_answer(a);
    const uint8_t server_minor = TMX_EXT_MINOR;
    if (payload[1] == TMX_EXT_MAJOR) {
        uint32_t errors = a->session.error_reports;     /* a lifetime counter: a new session keeps it */
        session_reset(a);
        a->session.error_reports = errors;
        a->session.extended = true;
        a->session.minor = payload[2] < server_minor ? payload[2] : server_minor;
        a->session.mode_changed = true;
    }
    return true;
}

/** SYNC [tag_hi, tag_lo, flags]: the reply carries the SYNC's own number; the flags govern what follows. */
static telemetrix_reason_t sync_command(tmx_adapter_t *a, uint16_t seq, const uint8_t *payload, size_t len)
{
    if (len < 3) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n = tmx_rpt_sync_reply(out, (uint16_t)(payload[0] << 8 | payload[1]), seq);
    a->emit(out, n, true, TMX_COALESCE_NONE, 0, a->ctx);
    a->session.abort = (payload[2] & 1) != 0;
    a->session.skipping = false;
    return TELEMETRIX_REASON_NONE;
}

static bool is_i2c_id(uint8_t id)
{
    return id >= TMX_CMD_I2C_BEGIN && id <= TMX_CMD_I2C_WRITE;
}

static bool is_onewire_id(uint8_t id)
{
    return id >= TMX_CMD_ONE_WIRE_INIT && id <= TMX_CMD_ONE_WIRE_CRC8;
}

static bool bus_in_profile(const tmx_adapter_t *a, uint8_t id, uint8_t bus)
{
    if (is_i2c_id(id)) {
        for (size_t i = 0; i < a->profile->n_i2c; i++) {
            if (a->profile->i2c[i].bus == bus) {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i < a->profile->n_onewire; i++) {
        if (a->profile->onewire[i].bus == bus) {
            return true;
        }
    }
    return false;
}

/** ON_BUS [bus, stock_id, stock payload...]: one stock I2C or 1-wire command on the chosen bus; its replies come back
 *  as BUS_REPORT and its refusal as an ERROR of ON_BUS. */
static telemetrix_reason_t on_bus_command(tmx_adapter_t *a, const uint8_t *payload, size_t len)
{
    if (len < 2 || !(is_i2c_id(payload[1]) || is_onewire_id(payload[1]))) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    const uint8_t bus = payload[0];
    const uint8_t id = payload[1];
    const uint8_t *inner = payload + 2;
    const size_t inner_len = len - 2;
    if (tmx_stock_check(id, inner, inner_len) != TMX_STOCK_OK) {
        return TELEMETRIX_REASON_BAD_ARGUMENT;
    }
    if (tmx_adapter_switched_off(id, inner)) {
        return TELEMETRIX_REASON_NOT_IN_SCOPE;
    }
    if (!bus_in_profile(a, id, bus)) {
        return TELEMETRIX_REASON_BUS_UNKNOWN;
    }
    a->on_bus = bus;
    telemetrix_reason_t why = is_i2c_id(id) ? tmx_i2c_command(a, bus, id, inner, inner_len) :
                              tmx_onewire_command(a, bus, id, inner, inner_len);
    a->on_bus = -1;
    return why;
}

/** The ids 57-60 and every stock id; the packet has a number and is not skipped. *code is the plugin's own code when
 *  a PLUGIN command is refused with PLUGIN. */
static telemetrix_reason_t extended_command(tmx_adapter_t *a, uint16_t seq, uint8_t id, const uint8_t *payload,
                                            size_t len, uint32_t now_ms, uint8_t *code)
{
    switch (id) {
    case TMX_CMD_GET_FIRMWARE_VERSION:
        emit_long_answer(a);
        return TELEMETRIX_REASON_NONE;
    case TMX_CMD_SYNC:
        return sync_command(a, seq, payload, len);
    case TMX_CMD_ON_BUS:
        return on_bus_command(a, payload, len);
    case TMX_CMD_PLUGIN:
        return tmx_adapter_plugin_command(a, payload, len, code);
    case TMX_CMD_GET_CAPABILITIES:
        return TELEMETRIX_REASON_NOT_IN_SCOPE;      /* placeholder: the handler of 59 replaces this */
    default:
        return tmx_adapter_command(a, id, payload, len, now_ms);
    }
}

static void extended_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms)
{
    if (id > TMX_CMD_EXT_LAST) {
        a->session.violation = true;
        return;
    }
    uint16_t seq = a->session.seq++;
    telemetrix_reason_t why = TELEMETRIX_REASON_SKIPPED;
    uint8_t code = 0;
    if (!a->session.skipping || id == TMX_CMD_SYNC) {
        why = extended_command(a, seq, id, payload, len, now_ms, &code);
    }
    if (why == TELEMETRIX_REASON_NONE) {
        return;
    }
    emit_error(a, seq, id, why, payload, len, code);
    if (why != TELEMETRIX_REASON_NOT_IN_SCOPE && why != TELEMETRIX_REASON_SKIPPED) {
        a->refused_commands++;
    }
    if (why != TELEMETRIX_REASON_SKIPPED && id != TMX_CMD_SYNC && a->session.abort) {
        a->session.skipping = true;
    }
}

void tmx_adapter_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms)
{
    if (a->session.extended) {
        extended_packet(a, id, payload, len, now_ms);
        return;
    }
    if (id == TMX_CMD_GET_FIRMWARE_VERSION && handshake(a, payload, len)) {
        return;
    }
    tmx_adapter_stock_refusal(a, id, payload, tmx_adapter_command(a, id, payload, len, now_ms));
}
