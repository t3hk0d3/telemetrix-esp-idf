#include <string.h>

#include "fake/tmx_drv_fake.h"
#include "tmx_core.h"
#include "unity.h"

static const tmx_drv_t *d = &tmx_drv_fake;

TEST_CASE("On a loopback while out is an output: in reads the driven level", "[fake]")
{
    tmx_fake_reset();
    tmx_fake_loopback(4, 5);
    d->gpio_input(5, false, false);
    d->gpio_output(4, true);
    TEST_ASSERT_EQUAL(1, d->gpio_get(5));
    d->gpio_set(4, false);
    TEST_ASSERT_EQUAL(0, d->gpio_get(5));
}

TEST_CASE("On a loopback while out is not driving: in reads its own pull level", "[fake]")
{
    tmx_fake_reset();
    tmx_fake_loopback(4, 5);
    tmx_fake_set_level(5, true);
    d->gpio_input(4, false, false);
    d->gpio_input(5, false, false);
    TEST_ASSERT_EQUAL(0, d->gpio_get(5));
    d->gpio_input(5, true, false);
    TEST_ASSERT_EQUAL(1, d->gpio_get(5));
    d->gpio_input(5, false, true);
    TEST_ASSERT_EQUAL(0, d->gpio_get(5));
    d->gpio_output(4, true);
    d->gpio_input(4, false, false);
    d->gpio_input(5, true, false);
    TEST_ASSERT_EQUAL(1, d->gpio_get(5));
}

TEST_CASE("On no loopback: an input reads the level set by set_level, default 0", "[fake]")
{
    tmx_fake_reset();
    d->gpio_input(7, false, false);
    TEST_ASSERT_EQUAL(0, d->gpio_get(7));
    tmx_fake_set_level(7, true);
    TEST_ASSERT_EQUAL(1, d->gpio_get(7));
    d->gpio_input(7, false, true);
    TEST_ASSERT_EQUAL(1, d->gpio_get(7));
}

TEST_CASE("On a name that is a prefix of another call: tmx_fake_count matches whole names only", "[fake]")
{
    uint8_t buf[1];
    tmx_fake_reset();
    uint8_t regs[2] = {1, 2};
    tmx_fake_i2c_device(0, 0x10, regs, 2);
    d->i2c_read(0, 0x10, 0, true, buf, 1, 50);
    d->i2c_write(0, 0x10, buf, 1, 50);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_read"));
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_rea"));
    TEST_ASSERT_EQUAL(0, tmx_fake_count("i2c_read_x"));
    TEST_ASSERT_EQUAL(1, tmx_fake_count("i2c_write"));
}

TEST_CASE("On reset: calls, levels, loopbacks and queued failures are forgotten", "[fake]")
{
    tmx_fake_reset();
    tmx_fake_set_level(3, true);
    tmx_fake_loopback(1, 3);
    tmx_fake_fail_next("gpio_input", ESP_FAIL);
    d->gpio_output(1, true);
    TEST_ASSERT_TRUE(strlen(tmx_fake_calls()) > 0);
    tmx_fake_reset();
    TEST_ASSERT_EQUAL_STRING("", tmx_fake_calls());
    TEST_ASSERT_EQUAL(0, d->gpio_get(3));
    TEST_ASSERT_EQUAL(ESP_OK, d->gpio_input(3, false, false));
    TEST_ASSERT_FALSE(tmx_fake_log_truncated());
}

TEST_CASE("On fail_next: the next call of that op returns the error once and is recorded", "[fake]")
{
    tmx_fake_reset();
    tmx_fake_fail_next("gpio_output", ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL(ESP_OK, d->gpio_input(1, false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, d->gpio_output(1, true));
    TEST_ASSERT_EQUAL(ESP_OK, d->gpio_output(1, true));
    TEST_ASSERT_EQUAL(2, tmx_fake_count("gpio_output"));
}

TEST_CASE("On fail_next for each op returning esp_err_t: it fails once", "[fake]")
{
    uint8_t b[1] = {0};
    int raw;
    tmx_fake_reset();
    tmx_fake_fail_next("ledc_timer", ESP_FAIL);
    tmx_fake_fail_next("ledc_attach", ESP_FAIL);
    tmx_fake_fail_next("ledc_duty", ESP_FAIL);
    tmx_fake_fail_next("adc_read", ESP_FAIL);
    tmx_fake_fail_next("i2c_open", ESP_FAIL);
    tmx_fake_fail_next("i2c_read", ESP_FAIL);
    tmx_fake_fail_next("i2c_write", ESP_FAIL);
    tmx_fake_fail_next("ow_open", ESP_FAIL);
    tmx_fake_fail_next("dht_start", ESP_FAIL);
    tmx_fake_fail_next("sonar_start", ESP_FAIL);
    tmx_fake_fail_next("init", ESP_FAIL);
    TEST_ASSERT_EQUAL(ESP_FAIL, d->ledc_timer(0, 0, 100, 8));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->ledc_attach(0, 0, 0, 2));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->ledc_duty(0, 0, 1));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->adc_read(1, 1, &raw));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->i2c_open(0, NULL, 1, 2));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->i2c_read(0, 1, 0, true, b, 1, 10));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->i2c_write(0, 1, b, 1, 10));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->ow_open(0, 1));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->dht_start(1, 1));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->sonar_start(1, 2, 1));
    TEST_ASSERT_EQUAL(ESP_FAIL, d->init(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, d->ledc_timer(0, 0, 100, 8));
    TEST_ASSERT_EQUAL(ESP_OK, d->adc_read(1, 1, &raw));
    TEST_ASSERT_EQUAL(ESP_OK, d->dht_start(1, 1));
}

TEST_CASE("On a full log: the truncation is visible and the log is not corrupted", "[fake]")
{
    tmx_fake_reset();
    for (int i = 0; i < 2000; i++) {
        d->gpio_output(1, true);
    }
    TEST_ASSERT_TRUE(tmx_fake_log_truncated());
    size_t n = strlen(tmx_fake_calls());
    TEST_ASSERT_TRUE(n > 0 && tmx_fake_calls()[n - 1] == '\n');
    tmx_fake_reset();
    TEST_ASSERT_FALSE(tmx_fake_log_truncated());
}

TEST_CASE("On dht_start and sonar_start without complete_pending: nothing is posted", "[fake]")
{
    tmx_completion_t c;
    tmx_fake_reset();
    tmx_completion_reset();
    TEST_ASSERT_EQUAL(ESP_OK, d->dht_start(4, 7));
    TEST_ASSERT_EQUAL(ESP_OK, d->sonar_start(5, 6, 8));
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
}

TEST_CASE("On complete_pending after a dht_start: the scripted frame is posted with the token", "[fake]")
{
    tmx_completion_t c;
    const uint8_t frame[5] = {1, 2, 3, 4, 10};
    tmx_fake_reset();
    tmx_completion_reset();
    tmx_fake_dht_frame(4, frame, ESP_OK);
    d->dht_start(4, 0x1234);
    tmx_fake_complete_pending();
    TEST_ASSERT_TRUE(tmx_completion_take(&c));
    TEST_ASSERT_EQUAL(TMX_COMPLETION_DHT, c.kind);
    TEST_ASSERT_EQUAL_UINT32(0x1234, c.token);
    TEST_ASSERT_EQUAL(ESP_OK, c.err);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, c.data, 5);
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
}

TEST_CASE("On a scripted DHT error: complete_pending posts that error", "[fake]")
{
    tmx_completion_t c;
    const uint8_t frame[5] = {0};
    tmx_fake_reset();
    tmx_completion_reset();
    tmx_fake_dht_frame(4, frame, ESP_ERR_TIMEOUT);
    d->dht_start(4, 1);
    tmx_fake_complete_pending();
    TEST_ASSERT_TRUE(tmx_completion_take(&c));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, c.err);
}

TEST_CASE("On complete_pending after a sonar_start: the scripted echo is posted; 0 us is a timeout", "[fake]")
{
    tmx_completion_t c;
    tmx_fake_reset();
    tmx_completion_reset();
    tmx_fake_sonar_echo(5, 5800);
    d->sonar_start(5, 6, 0x42);
    d->sonar_start(9, 10, 0x43);
    tmx_fake_complete_pending();
    TEST_ASSERT_TRUE(tmx_completion_take(&c));
    TEST_ASSERT_EQUAL(TMX_COMPLETION_SONAR, c.kind);
    TEST_ASSERT_EQUAL_UINT32(0x42, c.token);
    TEST_ASSERT_EQUAL(ESP_OK, c.err);
    TEST_ASSERT_EQUAL_UINT32(5800, c.value);
    TEST_ASSERT_TRUE(tmx_completion_take(&c));
    TEST_ASSERT_EQUAL_UINT32(0x43, c.token);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, c.err);
}

TEST_CASE("On several pending starts: complete_pending posts them in start order, once", "[fake]")
{
    tmx_completion_t c;
    tmx_fake_reset();
    tmx_completion_reset();
    d->sonar_start(5, 6, 1);
    d->dht_start(4, 2);
    d->sonar_start(7, 8, 3);
    tmx_fake_complete_pending();
    for (uint32_t want = 1; want <= 3; want++) {
        TEST_ASSERT_TRUE(tmx_completion_take(&c));
        TEST_ASSERT_EQUAL_UINT32(want, c.token);
    }
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
    tmx_fake_complete_pending();
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
}

TEST_CASE("On a dht_start the test made fail: nothing is pending for it", "[fake]")
{
    tmx_completion_t c;
    tmx_fake_reset();
    tmx_completion_reset();
    tmx_fake_fail_next("dht_start", ESP_FAIL);
    TEST_ASSERT_EQUAL(ESP_FAIL, d->dht_start(4, 1));
    tmx_fake_complete_pending();
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
}

TEST_CASE("On tmx_fake_reset: pending starts are forgotten", "[fake]")
{
    tmx_completion_t c;
    tmx_fake_reset();
    tmx_completion_reset();
    d->dht_start(4, 1);
    tmx_fake_reset();
    tmx_fake_complete_pending();
    TEST_ASSERT_FALSE(tmx_completion_take(&c));
}
