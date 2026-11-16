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

static void emit_error(tmx_adapter_t *a, uint16_t seq, uint8_t cmd, telemetrix_reason_t why)
{
    uint8_t out[TMX_EXT_REPORT_MAX];
    size_t n = tmx_rpt_error(out, seq, cmd, (uint8_t)why, -1, 0, -1);
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
        session_reset(a);
        a->session.extended = true;
        a->session.minor = payload[2] < server_minor ? payload[2] : server_minor;
        a->session.mode_changed = true;
    }
    return true;
}

/** The ids 57-60 and every stock id; the packet has a number and is not skipped. */
static telemetrix_reason_t extended_command(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len,
                                            uint32_t now_ms)
{
    if (id == TMX_CMD_GET_FIRMWARE_VERSION) {
        emit_long_answer(a);
        return TELEMETRIX_REASON_NONE;
    }
    if (id >= TMX_CMD_SYNC) {
        return TELEMETRIX_REASON_NOT_IN_SCOPE;      /* placeholder: the handlers of 57-60 replace this */
    }
    return tmx_adapter_command(a, id, payload, len, now_ms);
}

static void extended_packet(tmx_adapter_t *a, uint8_t id, const uint8_t *payload, size_t len, uint32_t now_ms)
{
    if (id > TMX_CMD_EXT_LAST) {
        a->session.violation = true;
        return;
    }
    uint16_t seq = a->session.seq++;
    telemetrix_reason_t why = TELEMETRIX_REASON_SKIPPED;
    if (!a->session.skipping || id == TMX_CMD_SYNC) {
        why = extended_command(a, id, payload, len, now_ms);
    }
    if (why == TELEMETRIX_REASON_NONE) {
        return;
    }
    emit_error(a, seq, id, why);
    if (why != TELEMETRIX_REASON_NOT_IN_SCOPE && why != TELEMETRIX_REASON_SKIPPED) {
        a->refused_commands++;
    }
    if (why != TELEMETRIX_REASON_SKIPPED && a->session.abort) {
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
