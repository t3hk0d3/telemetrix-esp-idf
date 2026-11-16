#include "adapter_fixture.h"
#include "test_plugin.h"

#define ERR(seq, cmd, why) 5, TMX_RPT_ERROR, (uint8_t)((seq) >> 8), (uint8_t)(seq), (cmd), (why)

static const telemetrix_i2c_bus_t i2c0 = {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000};
static const telemetrix_i2c_bus_t i2c_two[] = {
    {.bus = 0, .port = 0, .sda = 21, .scl = 22, .speed_hz = 100000},
    {.bus = 1, .port = 1, .sda = -1, .scl = -1, .speed_hz = 400000},
};
static const telemetrix_onewire_bus_t ow0 = {.bus = 0, .gpio = -1};

/** The records of every page, in order, with each page checked on the way. */
typedef struct {
    uint8_t rec[2048];
    size_t n;
    size_t pages;
} reassembled_t;

static void send_page(uint8_t page)
{
    fx_n_reports = 0;
    fx_send(TMX_CMD_GET_CAPABILITIES, &page, 1, 0);
}

static void reassemble(reassembled_t *r)
{
    r->n = 0;
    r->pages = 0;
    uint8_t last = 0;
    for (uint8_t page = 0; page <= last; page++) {
        send_page(page);
        TEST_ASSERT_EQUAL(1, fx_n_reports);
        const fx_report_t *rep = &fx_reports[0];
        TEST_ASSERT_TRUE(rep->reply);
        TEST_ASSERT_TRUE(rep->n <= 255);
        TEST_ASSERT_EQUAL_UINT8(rep->n - 1, rep->data[0]);
        TEST_ASSERT_EQUAL_UINT8(TMX_RPT_CAPABILITIES, rep->data[1]);
        TEST_ASSERT_EQUAL_UINT8(page, rep->data[2]);
        last = rep->data[3];
        TEST_ASSERT_TRUE(rep->n - 4 <= 251);
        size_t o = 4;
        while (o < rep->n) {                       /* no record split */
            TEST_ASSERT_TRUE(o + 2 <= rep->n);
            o += 2 + (size_t)rep->data[o + 1];
        }
        TEST_ASSERT_EQUAL_size_t(rep->n, o);
        memcpy(r->rec + r->n, rep->data + 4, rep->n - 4);
        r->n += rep->n - 4;
        r->pages++;
    }
}

/** Offset of the next record of type from off, or n when there is none. */
static size_t find(const reassembled_t *r, uint8_t type, size_t off)
{
    while (off < r->n) {
        if (r->rec[off] == type) {
            return off;
        }
        off += 2 + r->rec[off + 1];
    }
    return r->n;
}

static size_t count(const reassembled_t *r, uint8_t type)
{
    size_t c = 0;
    for (size_t o = find(r, type, 0); o < r->n; o = find(r, type, o + 2 + r->rec[o + 1])) {
        c++;
    }
    return c;
}

static void start_ext_with(const telemetrix_i2c_bus_t *i2c, size_t n_i2c, const telemetrix_onewire_bus_t *ow,
                           const telemetrix_plugin_t *const *plugins, size_t n_plugins)
{
    fx_start_buses(NULL, NULL);
    fx_profile.i2c = i2c;
    fx_profile.n_i2c = n_i2c;
    fx_profile.onewire = ow;
    fx_profile.n_onewire = ow ? 1 : 0;
    fx_cfg.extended = true;
    fx_cfg.plugins = plugins;
    fx_cfg.n_plugins = n_plugins;
    fx_n_reports = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    TEST_ASSERT_TRUE(tmx_adapter_extended(&fx_a));
    fx_n_reports = 0;
}

TEST_CASE("GET_CAPABILITIES [0] on the fixture profile: CHIP first with the fake chip, then PIN in profile order, "
          "BUS I2C then 1-wire, LIMIT 1-6, as one reply", "[discovery]")
{
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    reassembled_t r;
    reassemble(&r);
    TEST_ASSERT_EQUAL_size_t(1, r.pages);
    /* CHIP: model 0xFFFF, revision 0, "linux" */
    const uint8_t chip[] = {1, 9, 0xFF, 0xFF, 0, 0, 'l', 'i', 'n', 'u', 'x'};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(chip, r.rec, sizeof(chip));
    size_t o = sizeof(chip);
    for (size_t i = 0; i < fx_profile.n_pins; i++) {    /* PIN records, profile order */
        const telemetrix_pin_t *p = &fx_pins[i];
        TEST_ASSERT_EQUAL_UINT8(2, r.rec[o]);
        TEST_ASSERT_EQUAL_UINT8(6, r.rec[o + 1]);
        TEST_ASSERT_EQUAL_UINT8(p->gpio, r.rec[o + 2]);
        TEST_ASSERT_EQUAL_UINT8(p->caps >> 8, r.rec[o + 3]);
        TEST_ASSERT_EQUAL_UINT8(p->caps & 0xFF, r.rec[o + 4]);
        TEST_ASSERT_EQUAL_UINT8(p->safe, r.rec[o + 5]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)p->adc_unit, r.rec[o + 6]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)p->adc_channel, r.rec[o + 7]);
        o += 8;
    }
    const uint8_t buses[] = {
        3, 8, 0, 0, 21, 22, 0x00, 0x01, 0x86, 0xA0,        /* I2C bus 0, 100 kHz */
        3, 3, 1, 0, 0xFF,                                  /* 1-wire bus 0, the client chooses */
        4, 3, 1, 0, 4,                                     /* servos: min(max_servo 4, 8 channels) */
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(buses, r.rec + o, sizeof(buses));
}

TEST_CASE("GET_CAPABILITIES LIMIT records: kinds 1-6 in order from the grants", "[discovery]")
{
    start_ext_with(NULL, 0, NULL, NULL, 0);
    fx_a.cfg.max_servo = 3;
    fx_a.cfg.max_dht = 2;
    fx_a.cfg.max_sonar = 4;
    fx_a.cfg.analog_interval_floor_ms = 9;
    reassembled_t r;
    reassemble(&r);
    size_t o = find(&r, 4, 0);
    const uint8_t want[] = {
        4, 3, 1, 0, 3,      /* servos: min(max_servo 3, popcount(0xff) 8) */
        4, 3, 2, 0, 8,      /* PWM channels */
        4, 3, 3, 0, 4,      /* PWM timers: popcount(0x0f) */
        4, 3, 4, 0, 2,      /* DHT: an RMT RX channel is granted */
        4, 3, 5, 0, 4,      /* sonar */
        4, 3, 6, 0, 9,      /* analog floor */
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, r.rec + o, sizeof(want));
    TEST_ASSERT_EQUAL_size_t(6, count(&r, 4));
}

TEST_CASE("GET_CAPABILITIES on a profile with no RMT RX channel and few LEDC channels: DHT 0 and servos capped by "
          "channels", "[discovery]")
{
    start_ext_with(NULL, 0, NULL, NULL, 0);
    fx_profile.rmt.rx_channels = 0;
    fx_profile.ledc.channel_mask = 0x05;
    fx_a.cfg.max_servo = 12;
    fx_a.cfg.max_dht = 6;
    reassembled_t r;
    reassemble(&r);
    size_t o = find(&r, 4, 0);
    TEST_ASSERT_EQUAL_UINT8(1, r.rec[o + 2]);
    TEST_ASSERT_EQUAL_UINT8(2, r.rec[o + 4]);       /* min(12, 2) */
    o = find(&r, 4, o + 5);
    TEST_ASSERT_EQUAL_UINT8(2, r.rec[o + 4]);       /* PWM channels */
    o = find(&r, 4, o + 5);
    o = find(&r, 4, o + 5);
    TEST_ASSERT_EQUAL_UINT8(4, r.rec[o + 2]);
    TEST_ASSERT_EQUAL_UINT8(0, r.rec[o + 3]);
    TEST_ASSERT_EQUAL_UINT8(0, r.rec[o + 4]);       /* DHT */
}

TEST_CASE("GET_CAPABILITIES with two I2C buses, one shared and one the client chooses: both BUS records, profile "
          "order, -1 as 0xFF", "[discovery]")
{
    start_ext_with(i2c_two, 2, &ow0, NULL, 0);
    reassembled_t r;
    reassemble(&r);
    size_t o = find(&r, 3, 0);
    const uint8_t want[] = {
        3, 8, 0, 0, 21, 22, 0x00, 0x01, 0x86, 0xA0,
        3, 8, 0, 1, 0xFF, 0xFF, 0x00, 0x06, 0x1A, 0x80,
        3, 3, 1, 0, 0xFF,
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, r.rec + o, sizeof(want));
    TEST_ASSERT_EQUAL_size_t(3, count(&r, 3));
}

static const char *const cap_names[] = {"vendor.led", "vendor.motor", "other.horn"};
static const telemetrix_plugin_t pl_a = {
    .name = "vendor.alpha", .major = 1, .minor = 2, .named_caps = cap_names, .n_named_caps = 2,
};
static const telemetrix_plugin_t pl_b = {
    .name = "vendor.beta", .major = 3, .minor = 0, .named_caps = cap_names + 1, .n_named_caps = 2,
};

TEST_CASE("GET_CAPABILITIES with two plugins: NAMED_CAP records in id order after CHIP, PLUGIN records last in slot "
          "order", "[discovery]")
{
    const telemetrix_plugin_t *list[] = {&pl_a, &pl_b};
    start_ext_with(NULL, 0, NULL, list, 2);
    reassembled_t r;
    reassemble(&r);
    /* ids: vendor.led 0, vendor.motor 1, other.horn 2 */
    uint8_t want_head[] = {6, 11, 0, 'v', 'e', 'n', 'd', 'o', 'r', '.', 'l', 'e', 'd',
                           6, 13, 1, 'v', 'e', 'n', 'd', 'o', 'r', '.', 'm', 'o', 't', 'o', 'r',
                           6, 11, 2, 'o', 't', 'h', 'e', 'r', '.', 'h', 'o', 'r', 'n'
                          };
    /* the chip record is 11 bytes */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want_head, r.rec + 11, sizeof(want_head));
    const uint8_t tail[] = {
        5, 15, 0, 1, 2, 'v', 'e', 'n', 'd', 'o', 'r', '.', 'a', 'l', 'p', 'h', 'a',
        5, 14, 1, 3, 0, 'v', 'e', 'n', 'd', 'o', 'r', '.', 'b', 'e', 't', 'a',
    };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tail, r.rec + r.n - sizeof(tail), sizeof(tail));
    TEST_ASSERT_EQUAL_size_t(2, count(&r, 5));
    TEST_ASSERT_EQUAL_size_t(3, count(&r, 6));
}

TEST_CASE("GET_CAPABILITIES for a pin naming capabilities: its PIN record ends with the named cap ids", "[discovery]")
{
    static const char *const grants[] = {"other.horn", "vendor.led"};
    static telemetrix_pin_t pins[2];
    const telemetrix_plugin_t *list[] = {&pl_a, &pl_b};
    pins[0] = (telemetrix_pin_t) PIN(2, DIGITAL_CAPS, TELEMETRIX_SAFE_FLOAT, -1, -1);
    pins[0].named = grants;
    pins[0].n_named = 2;
    pins[1] = (telemetrix_pin_t) PIN(4, DIGITAL_CAPS, TELEMETRIX_SAFE_FLOAT, -1, -1);
    start_ext_with(NULL, 0, NULL, NULL, 0);
    fx_profile.pins = pins;
    fx_profile.n_pins = 2;
    fx_cfg.plugins = list;
    fx_cfg.n_plugins = 2;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    reassembled_t r;
    reassemble(&r);
    size_t o = find(&r, 2, 0);
    const uint8_t first[] = {2, 8, 2, 0x00, 0x0F, 0, 0xFF, 0xFF, 2, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, r.rec + o, sizeof(first));
    const uint8_t second[] = {2, 6, 4, 0x00, 0x0F, 0, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(second, r.rec + o + sizeof(first), sizeof(second));
}

/* 64 pins, each naming all four capabilities: the report spans pages. */
static const char *const four_names[] = {"v.a", "v.b", "v.c", "v.d"};
static const telemetrix_plugin_t pl_four = {
    .name = "vendor.four", .major = 1, .minor = 0, .named_caps = four_names, .n_named_caps = 4,
};

TEST_CASE("GET_CAPABILITIES on 64 pins naming 4 capabilities: several pages, every PIN record exactly once, no record "
          "split, every page at most 255 bytes", "[discovery]")
{
    static telemetrix_pin_t pins[64];
    for (size_t i = 0; i < 64; i++) {
        pins[i] = (telemetrix_pin_t) PIN(i, DIGITAL_CAPS | TELEMETRIX_CAP_PWM, TELEMETRIX_SAFE_FLOAT, -1, -1);
        pins[i].named = four_names;
        pins[i].n_named = 4;
    }
    const telemetrix_plugin_t *list[] = {&pl_four};
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    fx_profile.pins = pins;
    fx_profile.n_pins = 64;
    fx_cfg.plugins = list;
    fx_cfg.n_plugins = 1;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_adapter_init(&fx_a, &fx_profile, &fx_cfg, &tmx_drv_fake, fx_emit, NULL));
    FX_SEND(TMX_CMD_GET_FIRMWARE_VERSION, 0, TMX_EXT_MAGIC, 1, 0);
    reassembled_t r;
    reassemble(&r);
    TEST_ASSERT_TRUE(r.pages > 1);
    TEST_ASSERT_EQUAL_size_t(64, count(&r, 2));
    bool seen[64] = {false};
    for (size_t o = find(&r, 2, 0); o < r.n; o = find(&r, 2, o + 2 + r.rec[o + 1])) {
        const uint8_t gpio = r.rec[o + 2];
        TEST_ASSERT_TRUE(gpio < 64);
        TEST_ASSERT_FALSE(seen[gpio]);
        seen[gpio] = true;
        TEST_ASSERT_EQUAL_UINT8(10, r.rec[o + 1]);  /* 6 fixed bytes + 4 ids */
        const uint8_t ids[] = {0, 1, 2, 3};
        TEST_ASSERT_EQUAL_UINT8_ARRAY(ids, r.rec + o + 8, 4);
    }
    TEST_ASSERT_EQUAL_size_t(4, count(&r, 6));
    TEST_ASSERT_EQUAL_size_t(1, count(&r, 5));
    TEST_ASSERT_EQUAL_size_t(1, count(&r, 1));
}

TEST_CASE("GET_CAPABILITIES for a page past the last: ERROR BAD_ARGUMENT, one reply", "[discovery]")
{
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    fx_a.session.abort = false;
    send_page(1);
    const uint8_t e[] = {ERR(0, TMX_CMD_GET_CAPABILITIES, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e, sizeof(e));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(1, fx_refused());
}

TEST_CASE("GET_CAPABILITIES with no page byte: ERROR BAD_ARGUMENT", "[discovery]")
{
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    fx_a.session.abort = false;
    FX_SEND0(TMX_CMD_GET_CAPABILITIES, 0);
    const uint8_t e[] = {ERR(0, TMX_CMD_GET_CAPABILITIES, TELEMETRIX_REASON_BAD_ARGUMENT)};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, e, sizeof(e));
}

TEST_CASE("GET_CAPABILITIES twice: the same page both times, the profile read at request time", "[discovery]")
{
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    send_page(0);
    fx_report_t first = fx_reports[0];
    fx_a.cfg.max_sonar = 1;     /* a limit changed between the requests */
    send_page(0);
    TEST_ASSERT_EQUAL(first.n, fx_reports[0].n);
    TEST_ASSERT_FALSE(memcmp(first.data, fx_reports[0].data, first.n) == 0);
    reassembled_t r;
    reassemble(&r);
    size_t o = find(&r, 4, 0);
    o = find(&r, 4, o + 5);
    o = find(&r, 4, o + 5);
    o = find(&r, 4, o + 5);
    o = find(&r, 4, o + 5);
    TEST_ASSERT_EQUAL_UINT8(5, r.rec[o + 2]);
    TEST_ASSERT_EQUAL_UINT8(1, r.rec[o + 4]);
}

TEST_CASE("GET_CAPABILITIES in stock mode: ignored and counted, no report", "[discovery]")
{
    start_ext_with(&i2c0, 1, &ow0, NULL, 0);
    tmx_adapter_session_end(&fx_a);
    fx_n_reports = 0;
    FX_SEND(TMX_CMD_GET_CAPABILITIES, 0, 0);
    TEST_ASSERT_EQUAL(0, fx_n_reports);    TEST_ASSERT_EQUAL(1, fx_ignored());
}
