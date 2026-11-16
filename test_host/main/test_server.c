#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tmx_adapter.h"
#include "unity.h"

#define PORT 37411

static const telemetrix_pin_t pins[] = {
    {.gpio = 2, .caps = TELEMETRIX_CAP_DIN | TELEMETRIX_CAP_DOUT, .safe = TELEMETRIX_SAFE_DRIVE_LOW, .adc_unit = -1,
     .adc_channel = -1},
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

static telemetrix_profile_t profile = {.pins = pins, .n_pins = 1};

static void start_server(const telemetrix_config_t *cfg)
{
    hook_reset = hook_connected = hook_disconnected = hook_access = 0;
    access_allow = 1;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_start(&profile, cfg, &hooks));
}

static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT)};
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
    cfg.port = PORT;
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
    cfg.port = PORT;
    start_server(&cfg);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, telemetrix_start(&profile, &cfg, &hooks));

    telemetrix_status_t st;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_get_status(&st));
    TEST_ASSERT_TRUE(st.running);
    TEST_ASSERT_TRUE(st.listening);

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
    for (int i = 0; i < 20; i++) {
        vTaskDelay(1);
    }
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

    telemetrix_kick_client();
    TEST_ASSERT_EQUAL(-2, read_n(c1, &one, 1));        /* closed from the server side */
    close(c1);
    TEST_ASSERT_TRUE(wait_for(client_gone));
    TEST_ASSERT_EQUAL(1, hook_disconnected);

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
    cfg.port = PORT;
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
    cfg.port = PORT;
    start_server(&cfg);
    int c = dial();
    TEST_ASSERT_TRUE(wait_for(client_seen));
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
    TEST_ASSERT_EQUAL(1, hook_disconnected);
    uint8_t one;
    TEST_ASSERT_TRUE(read_n(c, &one, 1) == -2 || read_n(c, &one, 1) == -3);
    close(c);
    start_server(&cfg);                                /* the port is free again */
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_stop());
}
