#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "tmx_net.h"

#if CONFIG_IDF_TARGET_LINUX || CONFIG_LWIP_IPV6
#define HAVE_IPV6 1
#else
#define HAVE_IPV6 0
#endif

#if !CONFIG_IDF_TARGET_LINUX && !CONFIG_LWIP_SO_LINGER
#warning "telemetrix: CONFIG_LWIP_SO_LINGER is off, so refused connections are closed with FIN instead of RST; enable it"
#define LINGER_AVAILABLE 0
#else
#define LINGER_AVAILABLE 1
#endif

static const char *TAG = "telemetrix";

static int bind_listen(int family, uint16_t port)
{
    int fd = socket(family, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        return -1;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    int rc;
#if HAVE_IPV6
    if (family == AF_INET6) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof(a));        /* all zero is in6addr_any */
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    } else
#endif
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        rc = bind(fd, (struct sockaddr *)&a, sizeof(a));
    }
    if (rc == 0 && listen(fd, 1) == 0 && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0) {
        return fd;
    }
    close(fd);
    return -1;
}

int tmx_sock_listen(uint16_t port)
{
    static bool linger_logged;
    if (!LINGER_AVAILABLE && !linger_logged) {
        linger_logged = true;
        ESP_LOGW(TAG, "CONFIG_LWIP_SO_LINGER is off: refused connections close with FIN, not RST");
    }
    int fd = -1;
#if HAVE_IPV6
    fd = bind_listen(AF_INET6, port);
#endif
    if (fd < 0) {
        fd = bind_listen(AF_INET, port);
    }
    return fd;
}

/** Set an integer option; a value of 0 leaves the stack's default. */
static bool set_int(int fd, int level, int opt, int value)
{
    return value == 0 || setsockopt(fd, level, opt, &value, sizeof(value)) == 0;
}

esp_err_t tmx_sock_tune_client(int fd, const telemetrix_config_t *cfg)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || !set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1) ||
            !set_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1) ||
            !set_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, cfg->keepalive_idle_s) ||
            !set_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, cfg->keepalive_intvl_s) ||
            !set_int(fd, IPPROTO_TCP, TCP_KEEPCNT, cfg->keepalive_count)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void tmx_sock_refuse(int fd)
{
    struct linger l = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    close(fd);
}

static bool is_v4_mapped(const struct in6_addr *a)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(a->s6_addr, prefix, sizeof(prefix)) == 0;
}

void tmx_sock_normalise(const struct sockaddr_storage *in, struct sockaddr_storage *out)
{
#if HAVE_IPV6
    if (in->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)in;
        if (is_v4_mapped(&a6->sin6_addr)) {
            struct sockaddr_in a4;
            memset(&a4, 0, sizeof(a4));
            a4.sin_family = AF_INET;
            a4.sin_port = a6->sin6_port;
            memcpy(&a4.sin_addr, a6->sin6_addr.s6_addr + 12, 4);
            memset(out, 0, sizeof(*out));
            memcpy(out, &a4, sizeof(a4));
            return;
        }
    }
#endif
    *out = *in;
}

static bool prefix_match(const uint8_t *a, const uint8_t *b, unsigned bits)
{
    if (memcmp(a, b, bits / 8) != 0) {
        return false;
    }
    unsigned rest = bits % 8;
    if (rest == 0) {
        return true;
    }
    uint8_t mask = (uint8_t)(0xff << (8 - rest));
    return ((a[bits / 8] ^ b[bits / 8]) & mask) == 0;
}

bool tmx_sock_allowed(const telemetrix_config_t *cfg, const struct sockaddr *peer)
{
    if (cfg->n_allowed == 0) {
        return true;
    }
    const uint8_t *addr = NULL;
    size_t len = 0;
    if (peer->sa_family == AF_INET) {
        addr = (const uint8_t *) & ((const struct sockaddr_in *)peer)->sin_addr;
        len = 4;
    }
#if HAVE_IPV6
    else if (peer->sa_family == AF_INET6) {
        addr = ((const struct sockaddr_in6 *)peer)->sin6_addr.s6_addr;
        len = 16;
    }
#endif
    if (!addr) {
        return false;
    }
    for (size_t i = 0; i < cfg->n_allowed; i++) {
        const telemetrix_cidr_t *c = &cfg->allowed[i];
        if (c->family == peer->sa_family && c->prefix <= len * 8 && prefix_match(c->addr, addr, c->prefix)) {
            return true;
        }
    }
    return false;
}

void tmx_sock_peer_text(const struct sockaddr *peer, char out[TELEMETRIX_PEER_MAX])
{
    char ip[40];                       /* the longest plain IPv6 text, 39 characters */
    out[0] = '\0';
    if (peer->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)peer;
        if (inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip))) {
            snprintf(out, TELEMETRIX_PEER_MAX, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
        }
    }
#if HAVE_IPV6
    else if (peer->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)peer;
        if (inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip))) {
            snprintf(out, TELEMETRIX_PEER_MAX, "[%s]:%u", ip, (unsigned)ntohs(a->sin6_port));
        }
    }
#endif
}
