#include <string.h>

#include "fake/tmx_drv_fake.h"
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
