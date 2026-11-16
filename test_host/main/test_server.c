#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake/tmx_drv_fake.h"
#include "tmx_adapter.h"
#include "unity.h"

static uint16_t port;

/** A port nobody listens on right now: bind 0, read it back, close. Each test takes its own, so parallel runs differ. */
static uint16_t pick_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    socklen_t len = sizeof(a);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL(0, bind(fd, (struct sockaddr *)&a, sizeof(a)));
    TEST_ASSERT_EQUAL(0, getsockname(fd, (struct sockaddr *)&a, &len));
    close(fd);
    return ntohs(a.sin_port);
}

/* gpio 2 rests driven high and gpio 4 pulled up, so a safe state differs from what a client leaves behind */
static const telemetrix_pin_t pins[] = {
    {
        .gpio = 2, .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT, .safe = TELEMETRIX_SAFE_DRIVE_HIGH, .adc_unit = -1,
        .adc_channel = -1
    },
    {
        .gpio = 4, .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_PULL_UP, .safe = TELEMETRIX_SAFE_PULL_UP, .adc_unit = -1,
        .adc_channel = -1
    },
};

static int hook_reset, hook_connected, hook_disconnected, hook_access, access_allow;
static char hook_peer[TELEMETRIX_PEER_MAX];

static bool on_access(const struct sockaddr *peer, void *ctx)
{
    (void)peer, (void)ctx;
    hook_access++;
    return access_allow;
}

static void on_reset(void *ctx)
{
    (void)ctx;
    hook_reset++;
}

static void on_connected(const char *peer, void *ctx)
{
    (void)ctx;
    hook_connected++;
    strlcpy(hook_peer, peer, sizeof(hook_peer));
}

static void on_disconnected(void *ctx)
{
    (void)ctx;
    hook_disconnected++;
}

static const telemetrix_hooks_t hooks = {
    .access = on_access, .reset = on_reset, .connected = on_connected, .disconnected = on_disconnected,
};

static telemetrix_profile_t profile = {.pins = pins, .n_pins = 2};

static void start_server(const telemetrix_config_t *cfg)
{
    hook_reset = hook_connected = hook_disconnected = hook_access = 0;
    access_allow = 1;
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_start(&profile, cfg, &hooks));
}

static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL(0, connect(fd, (struct sockaddr *)&a, sizeof(a)));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

/** Read exactly n bytes, yielding to the server task between tries; -1 on error or timeout, -2 on end of stream. */
static int read_n(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    for (int tries = 0; tries < 300 && got < n; tries++) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return -2;
        } else if (errno == ECONNRESET) {
            return -3;
        } else {
            vTaskDelay(1);
        }
    }
    return got == n ? 0 : -1;
}

static bool wait_for(bool (*pred)(void))
{
    for (int i = 0; i < 300; i++) {
        if (pred()) {
            return true;
        }
        vTaskDelay(1);
    }
    return false;
}

static bool calls_have(const char *line)
{
    return strstr(tmx_fake_calls(), line) != NULL;
}

static void settle(void)
{
    for (int i = 0; i < 20; i++) {
        vTaskDelay(1);
    }
}

static bool client_seen(void)
{
    telemetrix_status_t st;
    return telemetrix_get_status(&st) == ESP_OK && st.client[0] != '\0';
}

static bool client_gone(void)
{
    telemetrix_status_t st;
    return telemetrix_get_status(&st) == ESP_OK && st.running && st.client[0] == '\0';
}

TEST_CASE("telemetrix_start on an invalid profile: ESP_ERR_INVALID_ARG and nothing started", "[server]")
{
    telemetrix_pin_t dup[2] = {pins[0], pins[0]};
    telemetrix_profile_t bad = {.pins = dup, .n_pins = 2};
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, telemetrix_start(&bad, &cfg, NULL));
    telemetrix_status_t st;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_FALSE(st.running);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_stop());
}

TEST_CASE("server session: start, a loop round trip, a second connection, kick, stop", "[server]")
{
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    start_server(&cfg);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_start(&profile, &cfg, &hooks));

    telemetrix_status_t st;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_TRUE(st.running);
    TEST_ASSERT_TRUE(st.listening);
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,1)"));          /* every profile pin in its safe state at start */
    TEST_ASSERT_TRUE(calls_have("gpio_input(4,1,0)"));

    int c1 = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    TEST_ASSERT_EQUAL(1, hook_connected);
    TEST_ASSERT_EQUAL_STRING_LEN("127.0.0.1:", hook_peer, 10);
    const uint8_t loop[] = {2, 0, 0x5a};
    TEST_ASSERT_EQUAL(3, send(c1, loop, sizeof(loop), 0));
    uint8_t reply[3];
    TEST_ASSERT_EQUAL(0, read_n(c1, reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(loop, reply, 3);

    const uint8_t mode_out[] = {3, 1, 2, 1};          /* SET_PIN_MODE gpio 2 output */
    TEST_ASSERT_EQUAL(4, send(c1, mode_out, sizeof(mode_out), 0));
    const uint8_t write_low[] = {3, 2, 2, 0};          /* DIGITAL_WRITE gpio 2 low */
    TEST_ASSERT_EQUAL(4, send(c1, write_low, sizeof(write_low), 0));
    settle();
    TEST_ASSERT_TRUE(calls_have("gpio_set(2,0)"));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_EQUAL(1, st.n_pins);
    TEST_ASSERT_EQUAL_STRING("output", st.pins[0].use);

    int c2 = dial();                                   /* refused with RST while c1 stays */
    uint8_t one;
    TEST_ASSERT_EQUAL(-3, read_n(c2, &one, 1));
    close(c2);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_EQUAL_UINT32(1, st.refused_connections);
    TEST_ASSERT_EQUAL(3, send(c1, loop, sizeof(loop), 0));
    TEST_ASSERT_EQUAL(0, read_n(c1, reply, sizeof(reply)));

    tmx_fake_reset();                                  /* only what the kick causes from here on */
    telemetrix_kick_client();
    TEST_ASSERT_EQUAL(-2, read_n(c1, &one, 1));        /* closed from the server side */
    close(c1);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(1, hook_disconnected);
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,1)"));  /* the claimed pin back in its safe state */
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_EQUAL(0, st.n_pins);

    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_FALSE(st.running);
}

TEST_CASE("server access: the allowed list and the access hook refuse before any byte is read", "[server]")
{
    telemetrix_cidr_t only_other;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("10.99.0.0/16", &only_other));
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    cfg.allowed = &only_other;
    cfg.n_allowed = 1;
    start_server(&cfg);
    only_other.addr[0] = 0;                            /* the copy must not follow the caller's array */

    int c = dial();
    uint8_t one;
    TEST_ASSERT_EQUAL(-3, read_n(c, &one, 1));
    close(c);
    TEST_ASSERT_EQUAL(0, hook_access);                 /* the list said no first */
    TEST_ASSERT_EQUAL(0, hook_connected);
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());

    telemetrix_cidr_t loopback;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse("127.0.0.0/8", &loopback));
    cfg.allowed = &loopback;
    start_server(&cfg);
    access_allow = 0;
    c = dial();
    TEST_ASSERT_EQUAL(-3, read_n(c, &one, 1));
    close(c);
    TEST_ASSERT_EQUAL(1, hook_access);
    TEST_ASSERT_EQUAL(0, hook_connected);
    access_allow = 1;
    c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    TEST_ASSERT_EQUAL(1, hook_connected);
    close(c);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
}

TEST_CASE("server stop with a client: pins released, the client closed, the hook called", "[server]")
{
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    start_server(&cfg);
    int c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    const uint8_t mode_out[] = {3, 1, 2, 1};
    TEST_ASSERT_EQUAL(4, send(c, mode_out, sizeof(mode_out), 0));
    settle();
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
    TEST_ASSERT_EQUAL(1, hook_disconnected);
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,1)"));  /* released: back to the safe state */
    uint8_t one;
    TEST_ASSERT_TRUE(read_n(c, &one, 1) == -2 || read_n(c, &one, 1) == -3);
    close(c);
    start_server(&cfg);                                /* the port is free again */
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
}

TEST_CASE("RESET twice and a write in one send: the hook runs twice and the write after the RESET takes effect", "[server]")
{
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    start_server(&cfg);
    int c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    const uint8_t mode_out[] = {3, 1, 2, 1};
    TEST_ASSERT_EQUAL(4, send(c, mode_out, sizeof(mode_out), 0));
    settle();
    tmx_fake_reset();
    /* RESET, RESET, SET_PIN_MODE gpio 2 output, DIGITAL_WRITE gpio 2 low, all in one chunk */
    const uint8_t chunk[] = {1, 20, 1, 20, 3, 1, 2, 1, 3, 2, 2, 0};
    TEST_ASSERT_EQUAL(sizeof(chunk), send(c, chunk, sizeof(chunk), 0));
    settle();
    TEST_ASSERT_EQUAL(2, hook_reset);
    const char *calls = tmx_fake_calls();
    const char *released = strstr(calls, "gpio_output(2,1)");   /* the RESET's safe state */
    const char *written = strstr(calls, "gpio_set(2,0)");
    TEST_ASSERT_NOT_NULL(released);
    TEST_ASSERT_NOT_NULL(written);
    TEST_ASSERT_TRUE(released < written);
    close(c);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
}

TEST_CASE("server with an application-owned I2C bus 0: start, a client reading it, disconnect and stop touch no bus pin",
          "[server]")
{
    static int app_bus;
    static const telemetrix_i2c_bus_t shared[] = {
        {.bus = 0, .port = 0, .sda = 25, .scl = 26, .speed_hz = 100000, .handle = &app_bus}
    };
    telemetrix_profile_t with_bus = profile;
    with_bus.i2c = shared;
    with_bus.n_i2c = 1;
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    hook_reset = hook_connected = hook_disconnected = hook_access = 0;
    access_allow = 1;
    tmx_fake_reset();
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_start(&with_bus, &cfg, &hooks));
    static const uint8_t regs[2] = {0x11, 0x22};
    tmx_fake_i2c_device(0, 0x76, regs, sizeof(regs));
    int c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    const uint8_t cmds[] = {1, 9, 5, 10, 0x76, 0, 2, 1};     /* I2C_BEGIN, I2C_READ 2 bytes of 0x76 from register 0 */
    TEST_ASSERT_EQUAL(sizeof(cmds), send(c, cmds, sizeof(cmds), 0));
    uint8_t reply[7];
    TEST_ASSERT_EQUAL(0, read_n(c, reply, sizeof(reply)));
    const uint8_t want[] = {6, 9, 2, 0x76, 0, 0x11, 0x22};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, reply, sizeof(want));
    close(c);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
    TEST_ASSERT_TRUE(calls_have("i2c_open(0,25,26,app)"));
    TEST_ASSERT_TRUE(calls_have("i2c_close(0)"));
    TEST_ASSERT_TRUE(calls_have("gpio_output(2,1)"));          /* the log holds the start's safe states */
    TEST_ASSERT_FALSE(calls_have("(25,"));
    TEST_ASSERT_FALSE(calls_have("(26,"));
}

/** The server's end of the connection whose client end is fd: the socket bound to the server port whose peer is fd's
 *  local address. -1 when there is none. */
static int server_end_of(int fd)
{
    struct sockaddr_in mine;
    socklen_t len = sizeof(mine);
    TEST_ASSERT_EQUAL(0, getsockname(fd, (struct sockaddr *)&mine, &len));
    for (int cand = 0; cand < 1024; cand++) {
        struct sockaddr_storage local, peer;
        socklen_t llen = sizeof(local), plen = sizeof(peer);
        if (cand == fd || getsockname(cand, (struct sockaddr *)&local, &llen) != 0 ||
                getpeername(cand, (struct sockaddr *)&peer, &plen) != 0) {
            continue;
        }
        uint16_t local_port = local.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&local)->sin6_port :
                              ((struct sockaddr_in *)&local)->sin_port;
        uint16_t peer_port = peer.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&peer)->sin6_port :
                             ((struct sockaddr_in *)&peer)->sin_port;
        if (ntohs(local_port) == port && peer_port == mine.sin_port) {
            return cand;
        }
    }
    return -1;
}

static int int_option(int fd, int level, int opt)
{
    int v = -1;
    socklen_t len = sizeof(v);
    TEST_ASSERT_EQUAL(0, getsockopt(fd, level, opt, &v, &len));
    return v;
}

TEST_CASE("server accepting a client: keepalive on with the configured idle, interval and count, TCP_NODELAY on",
          "[server]")
{
    telemetrix_config_t cfg;
    telemetrix_config_default(&cfg);
    port = pick_port();
    cfg.port = port;
    cfg.keepalive_idle_s = 7;                           /* not the defaults, so the values are the config's */
    cfg.keepalive_intvl_s = 3;
    cfg.keepalive_count = 4;
    start_server(&cfg);
    int c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    int srv = server_end_of(c);
    TEST_ASSERT_TRUE(srv >= 0);
    TEST_ASSERT_EQUAL(1, int_option(srv, SOL_SOCKET, SO_KEEPALIVE));
    TEST_ASSERT_EQUAL(7, int_option(srv, IPPROTO_TCP, TCP_KEEPIDLE));
    TEST_ASSERT_EQUAL(3, int_option(srv, IPPROTO_TCP, TCP_KEEPINTVL));
    TEST_ASSERT_EQUAL(4, int_option(srv, IPPROTO_TCP, TCP_KEEPCNT));
    TEST_ASSERT_EQUAL(1, int_option(srv, IPPROTO_TCP, TCP_NODELAY));
    close(c);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
}
