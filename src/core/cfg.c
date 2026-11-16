#include <string.h>

#include "telemetrix.h"

void telemetrix_config_default(telemetrix_config_t *out)
{
    if (!out) {
        return;
    }
    *out = (telemetrix_config_t) {
        .server_enabled = true,
        .port = 31336,
        .keepalive_idle_s = 10,
        .keepalive_intvl_s = 2,
        .keepalive_count = 3,
        .send_stall_ms = 10000,
        .send_queue_bytes = 4096,
        .i2c_timeout_ms = 50,
        .analog_interval_floor_ms = 10,
        .max_dht = 6,
        .max_sonar = 6,
        .max_servo = 4,
        .allowed = NULL,
        .n_allowed = 0,
        .task_priority = 5,
        .task_core = -1,
    };
}

/** Dotted quad of exactly four decimal fields of 1-3 digits, value 0-255, up to end. */
static bool parse_v4(const char *s, const char *end, uint8_t out[4])
{
    for (int f = 0; f < 4; f++) {
        unsigned v = 0, digits = 0;
        while (s < end && *s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            digits++;
            s++;
            if (digits > 3 || v > 255) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        out[f] = (uint8_t)v;
        if (f < 3) {
            if (s >= end || *s != '.') {
                return false;
            }
            s++;
        }
    }
    return s == end;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/** Colon-hex IPv6 up to end: at most one "::", an optional dotted IPv4 tail, 1-4 hex digits per group. */
static bool parse_v6(const char *s, const char *end, uint8_t out[16])
{
    uint16_t groups[8];
    int n = 0, gap = -1;                /* groups read; index at which "::" sat */
    if (s == end) {
        return false;
    }
    if (end - s >= 2 && s[0] == ':' && s[1] == ':') {
        gap = 0;
        s += 2;
    } else if (s[0] == ':') {
        return false;
    }
    while (s < end) {
        const char *p = s;
        unsigned v = 0, digits = 0;
        while (p < end && hex_value(*p) >= 0) {
            v = v * 16 + (unsigned)hex_value(*p);
            digits++;
            p++;
            if (digits > 4) {
                return false;
            }
        }
        if (p < end && *p == '.') {     /* dotted tail: the rest of the text is an IPv4 address in the last two groups */
            uint8_t v4[4];
            if (n > 6 || !parse_v4(s, end, v4)) {
                return false;
            }
            groups[n++] = (uint16_t)(v4[0] << 8 | v4[1]);
            groups[n++] = (uint16_t)(v4[2] << 8 | v4[3]);
            s = end;
            break;
        }
        if (digits == 0 || n >= 8) {
            return false;
        }
        groups[n++] = (uint16_t)v;
        s = p;
        if (s == end) {
            break;
        }
        if (*s != ':') {
            return false;
        }
        s++;
        if (s < end && *s == ':') {     /* "::" */
            if (gap >= 0) {
                return false;
            }
            gap = n;
            s++;
            if (s == end) {
                break;
            }
        } else if (s == end) {
            return false;               /* a single trailing colon */
        }
    }
    if (gap < 0 ? n != 8 : n > 7) {
        return false;
    }
    uint16_t full[8] = {0};
    if (gap < 0) {
        memcpy(full, groups, sizeof(full));
    } else {
        memcpy(full, groups, (size_t)gap * sizeof(uint16_t));
        memcpy(full + 8 - (n - gap), groups + gap, (size_t)(n - gap) * sizeof(uint16_t));
    }
    for (int i = 0; i < 8; i++) {
        out[2 * i] = (uint8_t)(full[i] >> 8);
        out[2 * i + 1] = (uint8_t)full[i];
    }
    return true;
}

esp_err_t telemetrix_cidr_parse(const char *text, telemetrix_cidr_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *slash = strchr(text, '/');
    const char *end = slash ? slash : text + strlen(text);
    bool v6 = memchr(text, ':', (size_t)(end - text)) != NULL;
    telemetrix_cidr_t c;
    memset(&c, 0, sizeof(c));
    unsigned max_prefix = v6 ? 128 : 32;
    c.family = v6 ? AF_INET6 : AF_INET;
    if (!(v6 ? parse_v6(text, end, c.addr) : parse_v4(text, end, c.addr))) {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned prefix = max_prefix;
    if (slash) {
        const char *p = slash + 1;
        unsigned digits = 0;
        prefix = 0;
        for (; *p >= '0' && *p <= '9'; p++) {
            prefix = prefix * 10 + (unsigned)(*p - '0');
            if (++digits > 3) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (digits == 0 || *p != '\0' || prefix > max_prefix) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    c.prefix = (uint8_t)prefix;
    *out = c;
    return ESP_OK;
}
