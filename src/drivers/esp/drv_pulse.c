#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#include "tmx_core.h"
#include "tmx_drv_esp.h"

/* DHT and sonar measurements. Both start without blocking and complete exactly once through tmx_completion_post: each
 * measurement owns an `active` flag that start sets and the first of {data callback, timeout timer} clears with an
 * exchange, so the loser of that race posts nothing. A timeout timer that fires late for an earlier measurement is
 * recognised by its deadline. */
static const char *TAG = "telemetrix";

#define DHT_START_LOW_US 1000
#define DHT_TIMEOUT_US 10000    /* from the release of the line */
#define DHT_IDLE_NS 200000      /* longer than any level of a frame, so the idle after the frame ends the receive */
#define DHT_GLITCH_NS 1000
#define DHT_BITS 40
#define DHT_ONE_US 50           /* a high pulse longer than this is a 1 */
#define RX_SYMBOLS SOC_RMT_MEM_WORDS_PER_CHANNEL
#define SONAR_TRIGGER_US 10
#define SONAR_TIMEOUT_US 25000

typedef struct {
    atomic_bool active;
    volatile int64_t deadline_us;   /* when the timeout timer of the running measurement may fire */
    uint32_t token;
    esp_timer_handle_t timeout;
} measure_t;

static void finish(measure_t *m, uint8_t kind, esp_err_t err, uint32_t value, const uint8_t *data)
{
    if (!atomic_exchange(&m->active, false)) {
        return;
    }
    tmx_completion_t c = {.kind = kind, .token = m->token, .err = err, .value = value};
    if (data) {
        memcpy(c.data, data, sizeof(c.data));
    }
    tmx_completion_post(&c);
}

/* A timer that was meant for an earlier measurement fires before the deadline of the running one. */
static bool timer_is_due(const measure_t *m)
{
    return esp_timer_get_time() + 500 >= m->deadline_us;
}

/* Begin a measurement: false when one is still running. */
static bool begin(measure_t *m, uint32_t token)
{
    if (atomic_exchange(&m->active, true)) {
        return false;
    }
    if (m->timeout) {
        esp_timer_stop(m->timeout);
    }
    m->deadline_us = INT64_MAX;
    m->token = token;
    return true;
}

static esp_err_t arm_timeout(measure_t *m, esp_timer_cb_t cb, const char *name, uint32_t us)
{
    if (!m->timeout) {
        const esp_timer_create_args_t args = {.callback = cb, .name = name};
        esp_err_t err = esp_timer_create(&args, &m->timeout);
        if (err != ESP_OK) {
            m->timeout = NULL;
            return err;
        }
    }
    m->deadline_us = esp_timer_get_time() + us;
    return esp_timer_start_once(m->timeout, us);
}

/* ---- DHT ---- */

static struct {
    measure_t m;
    rmt_channel_handle_t chan;
    int chan_gpio;
    uint8_t gpio;
    esp_timer_handle_t release;
    rmt_symbol_word_t symbols[RX_SYMBOLS];
} dht = {.chan_gpio = -1};

/* After the release the line shows: [high until the sensor answers] low 80, high 80, then 40 x (low 50, high 26 or 70),
 * then low 50 and the release. Flatten the pulses, keep the high ones that are followed by another pulse (the final
 * release high is not), decode the last 40 of those: a recorded release or response edge shifts nothing. */
static esp_err_t dht_decode(const rmt_symbol_word_t *sym, size_t count, uint8_t frame[5])
{
    uint8_t level[2 * RX_SYMBOLS];
    uint16_t us[2 * RX_SYMBOLS];
    size_t n = 0;
    for (size_t i = 0; i < count && i < RX_SYMBOLS; i++) {
        if (sym[i].duration0 == 0) {
            break;
        }
        level[n] = sym[i].level0;
        us[n++] = sym[i].duration0;
        if (sym[i].duration1 == 0) {
            break;
        }
        level[n] = sym[i].level1;
        us[n++] = sym[i].duration1;
    }
    size_t highs = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        highs += level[i] == 1;
    }
    if (highs < DHT_BITS) {
        return ESP_ERR_TIMEOUT;
    }
    size_t skip = highs - DHT_BITS;
    int bit = 0;
    memset(frame, 0, 5);
    for (size_t i = 0; i + 1 < n && bit < DHT_BITS; i++) {
        if (level[i] != 1) {
            continue;
        }
        if (skip > 0) {
            skip--;
            continue;
        }
        if (us[i] > DHT_ONE_US) {
            frame[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
        }
        bit++;
    }
    return ESP_OK;
}

static bool dht_recv_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ev, void *ctx)
{
    (void)ch, (void)ctx;
    uint8_t frame[5] = {0};
    esp_err_t err = dht_decode(ev->received_symbols, ev->num_symbols, frame);
    finish(&dht.m, TMX_COMPLETION_DHT, err, 0, frame);   /* the timeout timer is stopped by the next start */
    return false;
}

static void dht_free_channel(void)
{
    if (dht.chan) {
        rmt_disable(dht.chan);
        rmt_del_channel(dht.chan);
        dht.chan = NULL;
    }
    if (dht.chan_gpio >= 0) {
        esp_gpio_forget((uint8_t)dht.chan_gpio);
        dht.chan_gpio = -1;
    }
}

/* One RX channel serves every DHT; it is created on first use and re-created when a device on another pin reads. */
static esp_err_t dht_channel(uint8_t gpio)
{
    if (dht.chan && dht.chan_gpio == gpio) {
        return ESP_OK;
    }
    dht_free_channel();
    const rmt_rx_channel_config_t cfg = {
        .gpio_num = (gpio_num_t)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = RX_SYMBOLS,
    };
    esp_err_t err = rmt_new_rx_channel(&cfg, &dht.chan);
    if (err != ESP_OK) {
        dht.chan = NULL;
        return err;
    }
    const rmt_rx_event_callbacks_t cbs = {.on_recv_done = dht_recv_done};
    err = rmt_rx_register_event_callbacks(dht.chan, &cbs, NULL);
    if (err == ESP_OK) {
        err = rmt_enable(dht.chan);
    }
    if (err != ESP_OK) {
        rmt_del_channel(dht.chan);
        dht.chan = NULL;
        return err;
    }
    dht.chan_gpio = gpio;
    return ESP_OK;
}

static void dht_timeout_cb(void *arg)
{
    (void)arg;
    if (!atomic_load(&dht.m.active) || !timer_is_due(&dht.m)) {
        return;
    }
    if (dht.chan) {
        rmt_disable(dht.chan);   /* abandon the stuck reception */
        rmt_enable(dht.chan);
    }
    finish(&dht.m, TMX_COMPLETION_DHT, ESP_ERR_TIMEOUT, 0, NULL);
}

/* 1 ms after the start pulse: arm the receiver while the line is still low, then release it. */
static void dht_release_cb(void *arg)
{
    (void)arg;
    const rmt_receive_config_t rc = {.signal_range_min_ns = DHT_GLITCH_NS, .signal_range_max_ns = DHT_IDLE_NS};
    esp_err_t err = rmt_receive(dht.chan, dht.symbols, sizeof(dht.symbols), &rc);
    gpio_set_level((gpio_num_t)dht.gpio, 1);
    if (err == ESP_OK) {
        err = arm_timeout(&dht.m, dht_timeout_cb, "tmx_dht", DHT_TIMEOUT_US);
    }
    if (err != ESP_OK) {
        finish(&dht.m, TMX_COMPLETION_DHT, err, 0, NULL);
    }
}

esp_err_t esp_dht_start(uint8_t gpio, uint32_t token)
{
    if (!begin(&dht.m, token)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (!dht.release) {
        const esp_timer_create_args_t args = {.callback = dht_release_cb, .name = "tmx_dht_rel"};
        err = esp_timer_create(&args, &dht.release);
        if (err != ESP_OK) {
            dht.release = NULL;
        }
    }
    if (err == ESP_OK) {
        err = dht_channel(gpio);
    }
    if (err == ESP_OK) {
        /* After the channel: creating it configures the pin as an input. Open drain with the pull-up, the RMT input
         * still reads the line. */
        esp_gpio_forget(gpio);
        dht.gpio = gpio;
        gpio_set_level((gpio_num_t)gpio, 0);
        err = gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_INPUT_OUTPUT_OD);
        if (err == ESP_OK) {
            err = gpio_set_pull_mode((gpio_num_t)gpio, GPIO_PULLUP_ONLY);
        }
    }
    if (err == ESP_OK) {
        err = esp_timer_start_once(dht.release, DHT_START_LOW_US);
    }
    if (err != ESP_OK) {
        gpio_set_level((gpio_num_t)gpio, 1);
        atomic_store(&dht.m.active, false);
        ESP_LOGD(TAG, "dht on gpio %u: %s", gpio, esp_err_to_name(err));
    }
    return err;
}

/* ---- sonar ---- */

static struct {
    measure_t m;
    uint8_t echo;
    atomic_bool attached;       /* the echo handler is added */
    volatile int64_t rise_us;
    bool isr_service_ours;
} sonar;

static void echo_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (gpio_get_level((gpio_num_t)(intptr_t)arg)) {
        if (sonar.rise_us == 0) {
            sonar.rise_us = now;
        }
    } else if (sonar.rise_us != 0) {
        gpio_intr_disable((gpio_num_t)(intptr_t)arg);
        finish(&sonar.m, TMX_COMPLETION_SONAR, ESP_OK, (uint32_t)(now - sonar.rise_us), NULL);
    }
}

/* Remove the echo handler; safe from the timer, from start and from deinit. */
static void sonar_detach(void)
{
    if (atomic_exchange(&sonar.attached, false)) {
        gpio_intr_disable((gpio_num_t)sonar.echo);
        gpio_isr_handler_remove((gpio_num_t)sonar.echo);
        gpio_set_intr_type((gpio_num_t)sonar.echo, GPIO_INTR_DISABLE);
    }
}

static void sonar_timeout_cb(void *arg)
{
    (void)arg;
    if (!timer_is_due(&sonar.m)) {
        return;
    }
    sonar_detach();    /* after a completion by the echo too: the handler goes when the 25 ms are over */
    finish(&sonar.m, TMX_COMPLETION_SONAR, ESP_ERR_TIMEOUT, 0, NULL);
}

static esp_err_t isr_service(void)
{
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        sonar.isr_service_ours = true;
    }
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;   /* installed already */
}

esp_err_t esp_sonar_start(uint8_t trig, uint8_t echo, uint32_t token)
{
    if (!begin(&sonar.m, token)) {
        return ESP_ERR_INVALID_STATE;
    }
    sonar_detach();    /* the handler of the previous ping, if its timer has not run yet */
    esp_err_t err = isr_service();
    if (err == ESP_OK) {
        err = esp_gpio_input(echo, false, false);
    }
    if (err == ESP_OK) {
        err = esp_gpio_output(trig, false);
    }
    if (err == ESP_OK) {
        sonar.echo = echo;
        sonar.rise_us = 0;
        err = gpio_set_intr_type((gpio_num_t)echo, GPIO_INTR_ANYEDGE);
    }
    if (err == ESP_OK) {
        err = gpio_isr_handler_add((gpio_num_t)echo, echo_isr, (void *)(intptr_t)echo);
    }
    if (err != ESP_OK) {
        gpio_set_intr_type((gpio_num_t)echo, GPIO_INTR_DISABLE);
        atomic_store(&sonar.m.active, false);
        return err;
    }
    atomic_store(&sonar.attached, true);
    gpio_intr_enable((gpio_num_t)echo);
    esp_gpio_set(trig, true);
    esp_rom_delay_us(SONAR_TRIGGER_US);
    esp_gpio_set(trig, false);
    err = arm_timeout(&sonar.m, sonar_timeout_cb, "tmx_sonar", SONAR_TIMEOUT_US);
    if (err != ESP_OK) {
        sonar_detach();
        atomic_store(&sonar.m.active, false);
    }
    return err;
}

/* ---- release ---- */

static void stop_timer(esp_timer_handle_t *t)
{
    if (*t) {
        esp_timer_stop(*t);
        esp_timer_delete(*t);
        *t = NULL;
    }
}

void esp_pulse_deinit(void)
{
    stop_timer(&dht.release);
    stop_timer(&dht.m.timeout);
    atomic_store(&dht.m.active, false);
    if (dht.chan_gpio >= 0) {
        gpio_set_level((gpio_num_t)dht.chan_gpio, 1);
    }
    dht_free_channel();
    stop_timer(&sonar.m.timeout);
    sonar_detach();
    atomic_store(&sonar.m.active, false);
    if (sonar.isr_service_ours) {   /* an installation by the application stays */
        gpio_uninstall_isr_service();
        sonar.isr_service_ours = false;
    }
}
