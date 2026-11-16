#include <string.h>

#include "tmx_core.h"
#include "unity.h"

static const char *const caps_a[] = {"a.led", "a.motor"};
static const char *const caps_b[] = {"a.motor", "b.horn"};

static telemetrix_plugin_t plugin(const char *name)
{
    return (telemetrix_plugin_t) {
        .name = name, .major = 1, .minor = 0
    };
}

static esp_err_t init_one(const char *name)
{
    tmx_plugins_t r;
    const telemetrix_plugin_t p = plugin(name);
    const telemetrix_plugin_t *list[] = {&p};
    return tmx_plugins_init(&r, list, 1);
}

TEST_CASE("tmx_plugins_init on two plugins: slots 0 and 1 in registration order", "[plugins]")
{
    tmx_plugins_t r;
    const telemetrix_plugin_t a = plugin("vendor.alpha"), b = plugin("vendor.beta");
    const telemetrix_plugin_t *list[] = {&a, &b};
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 2));
    TEST_ASSERT_EQUAL(2, r.n);
    TEST_ASSERT_EQUAL_PTR(&a, r.ctx[0].plugin);
    TEST_ASSERT_EQUAL(0, r.ctx[0].slot);
    TEST_ASSERT_EQUAL_PTR(&b, r.ctx[1].plugin);
    TEST_ASSERT_EQUAL(1, r.ctx[1].slot);
}

TEST_CASE("tmx_plugins_init on no plugins: an empty registry", "[plugins]")
{
    tmx_plugins_t r;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, NULL, 0));
    TEST_ASSERT_EQUAL(0, r.n);
    TEST_ASSERT_EQUAL(0, r.n_named);
}

TEST_CASE("tmx_plugins_init on a duplicate name: ESP_ERR_INVALID_ARG", "[plugins]")
{
    tmx_plugins_t r;
    const telemetrix_plugin_t a = plugin("vendor.alpha"), b = plugin("vendor.alpha");
    const telemetrix_plugin_t *list[] = {&a, &b};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 2));
}

TEST_CASE("tmx_plugins_init on a bad name: ESP_ERR_INVALID_ARG", "[plugins]")
{
    static const char *const bad[] = {
        "", "nodot", ".lead", "trail.", "Upper.case", "sp ace.x", "a-b.c", "a.b-c", "x.y!",
        "abcdefghijklmnop.qrstuvwxyz012345",    /* 33 bytes */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, init_one(bad[i]), bad[i]);
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, init_one(NULL));
}

TEST_CASE("tmx_plugins_init on names at the edge of the pattern: accepted", "[plugins]")
{
    static const char *const good[] = {
        "a.b", "x_1.y.z", "a..b", "a.b.", "0.9", "abcdefghijklmnop.qrstuvwxyz01234",    /* the last is 32 bytes */
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, init_one(good[i]), good[i]);
    }
}

TEST_CASE("tmx_plugins_init on a reserved telemetrix. name: ESP_ERR_INVALID_ARG", "[plugins]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, init_one("telemetrix.mine"));
}

TEST_CASE("tmx_plugins_init on more than 8 plugins: ESP_ERR_INVALID_ARG", "[plugins]")
{
    static const char *const names[] = {"p.a", "p.b", "p.c", "p.d", "p.e", "p.f", "p.g", "p.h", "p.i"};
    telemetrix_plugin_t ps[9];
    const telemetrix_plugin_t *list[9];
    for (int i = 0; i < 9; i++) {
        ps[i] = plugin(names[i]);
        list[i] = &ps[i];
    }
    tmx_plugins_t r;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 8));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 9));
}

TEST_CASE("tmx_plugins_init on a NULL list or entry: ESP_ERR_INVALID_ARG", "[plugins]")
{
    tmx_plugins_t r;
    const telemetrix_plugin_t *list[] = {NULL};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, NULL, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 1));
}

TEST_CASE("tmx_plugins_init on named capabilities declared by two plugins: one id per distinct name, first "
          "declaration first", "[plugins]")
{
    tmx_plugins_t r;
    telemetrix_plugin_t a = plugin("vendor.alpha"), b = plugin("vendor.beta");
    a.named_caps = caps_a;
    a.n_named_caps = 2;
    b.named_caps = caps_b;
    b.n_named_caps = 2;
    const telemetrix_plugin_t *list[] = {&a, &b};
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 2));
    TEST_ASSERT_EQUAL(3, r.n_named);
    TEST_ASSERT_EQUAL_STRING("a.led", r.named[0]);
    TEST_ASSERT_EQUAL_STRING("a.motor", r.named[1]);
    TEST_ASSERT_EQUAL_STRING("b.horn", r.named[2]);
    TEST_ASSERT_EQUAL(1, tmx_plugins_named_id(&r, "a.motor"));
    TEST_ASSERT_EQUAL(2, tmx_plugins_named_id(&r, "b.horn"));
}

TEST_CASE("tmx_plugins_named_id of a name no plugin declares: -1", "[plugins]")
{
    tmx_plugins_t r;
    telemetrix_plugin_t a = plugin("vendor.alpha");
    a.named_caps = caps_a;
    a.n_named_caps = 2;
    const telemetrix_plugin_t *list[] = {&a};
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 1));
    TEST_ASSERT_EQUAL(-1, tmx_plugins_named_id(&r, "b.horn"));
}

TEST_CASE("tmx_plugins_init on more than 16 distinct named capabilities: ESP_ERR_INVALID_ARG", "[plugins]")
{
    static const char *const many[] = {
        "n.0", "n.1", "n.2", "n.3", "n.4", "n.5", "n.6", "n.7", "n.8", "n.9", "n.10", "n.11", "n.12", "n.13", "n.14",
        "n.15", "n.16",
    };
    tmx_plugins_t r;
    telemetrix_plugin_t a = plugin("vendor.alpha");
    a.named_caps = many;
    a.n_named_caps = 16;
    const telemetrix_plugin_t *list[] = {&a};
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 1));
    a.n_named_caps = 17;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 1));
}

TEST_CASE("tmx_plugins_init on a named capability over 24 bytes or NULL: ESP_ERR_INVALID_ARG", "[plugins]")
{
    static const char *const ok[] = {"abcdefghijkl.mnopqrstuvw"};       /* 24 bytes */
    static const char *const long_name[] = {"abcdefghijkl.mnopqrstuvwx"};  /* 25 bytes */
    static const char *const null_name[] = {NULL};
    tmx_plugins_t r;
    telemetrix_plugin_t a = plugin("vendor.alpha");
    const telemetrix_plugin_t *list[] = {&a};
    a.n_named_caps = 1;
    a.named_caps = ok;
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 1));
    a.named_caps = long_name;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 1));
    a.named_caps = null_name;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 1));
    a.named_caps = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tmx_plugins_init(&r, list, 1));
}

TEST_CASE("tmx_plugins_account over the budget: overruns +1 and max_us the longest call", "[plugins]")
{
    telemetrix_plugin_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    tmx_plugins_account(&ctx, 5000, 5000);
    TEST_ASSERT_EQUAL(0, ctx.overruns);
    tmx_plugins_account(&ctx, 5001, 5000);
    TEST_ASSERT_EQUAL(1, ctx.overruns);
    tmx_plugins_account(&ctx, 20, 5000);
    TEST_ASSERT_EQUAL(1, ctx.overruns);
    TEST_ASSERT_EQUAL(5001, ctx.max_us);
}

TEST_CASE("tmx_plugins_status: name, slot, counters and text per plugin in slot order, at most cap", "[plugins]")
{
    tmx_plugins_t r;
    const telemetrix_plugin_t a = plugin("vendor.alpha"), b = plugin("abcdefghijklmnop.qrstuvwxyz01234");
    const telemetrix_plugin_t *list[] = {&a, &b};
    TEST_ASSERT_EQUAL(ESP_OK, tmx_plugins_init(&r, list, 2));
    r.ctx[1].commands = 3;
    r.ctx[1].refusals = 1;
    r.ctx[1].overruns = 2;
    r.ctx[1].max_us = 7000;
    r.ctx[1].async_dropped = 4;
    strcpy(r.ctx[1].text, "warm");
    telemetrix_plugin_status_t out[3];
    memset(out, 0xa5, sizeof(out));
    TEST_ASSERT_EQUAL(2, tmx_plugins_status(&r, out, 3));
    TEST_ASSERT_EQUAL_STRING("vendor.alpha", out[0].name);
    TEST_ASSERT_EQUAL(0, out[0].slot);
    TEST_ASSERT_EQUAL(0, out[0].commands);
    TEST_ASSERT_EQUAL_STRING("", out[0].text);
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmnop.qrstuvwxyz01234", out[1].name);
    TEST_ASSERT_EQUAL(1, out[1].slot);
    TEST_ASSERT_EQUAL(3, out[1].commands);
    TEST_ASSERT_EQUAL(1, out[1].refusals);
    TEST_ASSERT_EQUAL(2, out[1].overruns);
    TEST_ASSERT_EQUAL(7000, out[1].max_us);
    TEST_ASSERT_EQUAL(4, out[1].async_dropped);
    TEST_ASSERT_EQUAL_STRING("warm", out[1].text);
    TEST_ASSERT_EQUAL(1, tmx_plugins_status(&r, out, 1));
}
