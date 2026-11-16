#include "adapter_fixture.h"

static const uint8_t frame_ok[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEE};   /* 65.2 %, 35.1 C */
static const uint8_t frame_bad[5] = {0x02, 0x8C, 0x01, 0x5F, 0xEF};
static const uint8_t frame_neg[5] = {0x02, 0x8C, 0x80, 0x65, 0x73};  /* 65.2 %, -10.1 C */

static void complete_and_poll(uint32_t now)
{
    tmx_fake_complete_pending();
    tmx_adapter_poll(&fx_a, now);
}

/* ---- sonar ---- */

TEST_CASE("SONAR_NEW: trigger claimed as an output, echo as an input, both reported as sonar", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_SONAR_TRIG, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL(TMX_USE_SONAR_ECHO, tmx_pin_use(&fx_a.claims, 13));
}

TEST_CASE("SONAR_NEW: the first ping starts at once on the next poll", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "sonar_start(12,13,"));
}

TEST_CASE("A sonar echo of 5800 us: [4,10,trig,0,100] once, the same distance again is not reported", "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    const uint8_t want[] = {4, 10, 12, 0, 100};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_FALSE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(TMX_COALESCE_SONAR, fx_reports[0].kind);
    TEST_ASSERT_EQUAL(0, fx_reports[0].key);
    tmx_adapter_poll(&fx_a, 33);                /* second ping */
    complete_and_poll(34);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("A sonar echo that changes the distance: reported again", "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    tmx_fake_sonar_echo(12, 5800 + 58 * 300);   /* 400 cm = 0x0190 */
    tmx_adapter_poll(&fx_a, 33);
    complete_and_poll(34);
    const uint8_t want[] = {4, 10, 12, 0x01, 0x90};
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    fx_assert_report(1, want, sizeof(want));
}

TEST_CASE("A sonar ping in progress: poll returns before the completion, the report appears after it", "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);                 /* returns although sonar_start has not completed */
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
    tmx_adapter_poll(&fx_a, 5);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    tmx_fake_complete_pending();
    TEST_ASSERT_EQUAL(0, fx_n_reports);         /* posted, not yet taken by the network task */
    tmx_adapter_poll(&fx_a, 6);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("A sonar ping in progress: no second ping starts while the first has not completed", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 14, 15);
    tmx_adapter_poll(&fx_a, 0);
    tmx_adapter_poll(&fx_a, 33);
    tmx_adapter_poll(&fx_a, 66);
    tmx_adapter_poll(&fx_a, 1000);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
}

TEST_CASE("Two sonars: one device per 33 ms, round robin", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 14, 15);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    tmx_adapter_poll(&fx_a, 32);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));    /* 33 ms have not passed */
    tmx_adapter_poll(&fx_a, 33);
    complete_and_poll(34);
    tmx_adapter_poll(&fx_a, 66);
    TEST_ASSERT_EQUAL(3, tmx_fake_count("sonar_start"));
    const char *c = tmx_fake_calls();
    const char *first = strstr(c, "sonar_start(12,13,");
    const char *second = strstr(c, "sonar_start(14,15,");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first < second);
    TEST_ASSERT_NOT_NULL(strstr(second + 1, "sonar_start(12,13,"));
}

TEST_CASE("A sonar echo timeout: no report, the next ping still starts", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);      /* echo 0 = timeout in the fake */
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    tmx_adapter_poll(&fx_a, 33);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("sonar_start"));
}

TEST_CASE("A sonar start the driver refuses: no report, the next ping is tried at the next slot", "[adapter]")
{
    fx_start();
    tmx_fake_fail_next("sonar_start", ESP_FAIL);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    tmx_adapter_poll(&fx_a, 33);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("sonar_start"));
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("SONAR_NEW over cfg.max_sonar: refused_commands +1, nothing claimed", "[adapter]")
{
    fx_start();
    fx_a.cfg.max_sonar = 1;
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 14, 15);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 14));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 15));
}

TEST_CASE("SONAR_NEW with an input-only trigger or a busy pin: refused_commands +1, nothing claimed", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 34, 13);      /* 34 has no output */
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 99);      /* 99 is no profile pin */
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 12);      /* trigger = echo */
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, TMX_MODE_OUTPUT);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);      /* echo busy */
    TEST_ASSERT_EQUAL(4, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 34));
}

TEST_CASE("A sonar pin another feature claimed first: SET_PIN_MODE on it is refused", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 13, TMX_MODE_INPUT, 0);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_SONAR_ECHO, tmx_pin_use(&fx_a.claims, 13));
}

/* ---- DHT ---- */

TEST_CASE("DHT_NEW: pin claimed as dht with one RMT RX, the first read starts at once", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    TEST_ASSERT_EQUAL(0, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_DHT, tmx_pin_use(&fx_a.claims, 14));
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
    TEST_ASSERT_NOT_NULL(strstr(tmx_fake_calls(), "dht_start(14,"));
}

TEST_CASE("A DHT22 frame 02 8C 01 5F EE: humidity 65.2, temperature 35.1 as [11,11,0,pin,hum,temp]", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_ok, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_dht_data(want, 14, 65.2f, 35.1f);
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, n);
    TEST_ASSERT_EQUAL(12, fx_reports[0].n);
    TEST_ASSERT_FALSE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(TMX_COALESCE_DHT, fx_reports[0].kind);
    TEST_ASSERT_EQUAL(0, fx_reports[0].key);
}

TEST_CASE("A DHT22 frame with the temperature sign bit set: a negative temperature", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_neg, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    uint8_t want[TMX_REPORT_MAX];
    size_t n = tmx_rpt_dht_data(want, 14, 65.2f, -10.1f);
    fx_assert_report(0, want, n);
}

TEST_CASE("A DHT frame with a bad checksum: [4,11,1,pin,1] as a reply, the device stays registered", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_bad, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    const uint8_t want[] = {4, 11, 1, 14, 1};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
    TEST_ASSERT_EQUAL(TMX_USE_DHT, tmx_pin_use(&fx_a.claims, 14));
    tmx_adapter_poll(&fx_a, 2000);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("dht_start"));
}

TEST_CASE("A DHT read that times out: [4,11,1,pin,2] as a reply", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_ok, ESP_ERR_TIMEOUT);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    const uint8_t want[] = {4, 11, 1, 14, 2};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    TEST_ASSERT_TRUE(fx_reports[0].reply);
}

TEST_CASE("A DHT start the driver refuses: [4,11,1,pin,2] as a reply, the device stays registered", "[adapter]")
{
    fx_start();
    tmx_fake_fail_next("dht_start", ESP_FAIL);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    const uint8_t want[] = {4, 11, 1, 14, 2};
    TEST_ASSERT_EQUAL(1, fx_n_reports);
    fx_assert_report(0, want, sizeof(want));
    tmx_adapter_poll(&fx_a, 2000);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("dht_start"));
}

TEST_CASE("DHT reads repeat every 2000 ms", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_ok, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 100, 14);
    tmx_adapter_poll(&fx_a, 100);
    complete_and_poll(101);
    tmx_adapter_poll(&fx_a, 2099);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
    tmx_adapter_poll(&fx_a, 2100);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("dht_start"));
}

TEST_CASE("Two DHTs: read one at a time, the second starts when the first completed", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_ok, ESP_OK);
    tmx_fake_dht_frame(15, frame_ok, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 15);
    TEST_ASSERT_EQUAL(1, fx_a.claims.rmt_rx_used);      /* one RX channel shared */
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
    tmx_adapter_poll(&fx_a, 10);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
    complete_and_poll(11);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("dht_start"));
    TEST_ASSERT_NOT_NULL(strstr(strstr(tmx_fake_calls(), "dht_start(14,"), "dht_start(15,"));
    complete_and_poll(12);
    TEST_ASSERT_EQUAL(2, fx_n_reports);
    TEST_ASSERT_EQUAL(14, fx_reports[0].data[3]);
    TEST_ASSERT_EQUAL(15, fx_reports[1].data[3]);
    TEST_ASSERT_EQUAL(1, fx_reports[1].key);
}

TEST_CASE("DHT_NEW over cfg.max_dht: refused_commands +1, nothing claimed", "[adapter]")
{
    fx_start();
    fx_a.cfg.max_dht = 1;
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 15);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 15));
}

TEST_CASE("DHT_NEW on an input-only pin or a busy pin: refused_commands +1, nothing claimed", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_DHT_NEW, 0, 34);            /* no output */
    FX_SEND(TMX_CMD_SET_PIN_MODE, 0, 14, TMX_MODE_OUTPUT);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    TEST_ASSERT_EQUAL(2, fx_refused());
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    TEST_ASSERT_EQUAL(TMX_USE_OUTPUT, tmx_pin_use(&fx_a.claims, 14));
}

TEST_CASE("DHT_NEW with no RMT RX channel: refused_commands +1, the pin is not claimed", "[adapter]")
{
    fx_start();
    TEST_ASSERT_EQUAL(ESP_OK, tmx_claim_rmt_rx(&fx_a.claims));
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    TEST_ASSERT_EQUAL(1, fx_refused());
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 14));
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("dht_start"));
}

/* ---- scheduling, reporting and release ---- */

TEST_CASE("poll: the return value includes the sonar deadline", "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    TEST_ASSERT_EQUAL_UINT32(32, tmx_adapter_poll(&fx_a, 1));
}

TEST_CASE("poll: the return value includes the DHT deadline", "[adapter]")
{
    fx_start();
    tmx_fake_dht_frame(14, frame_ok, ESP_OK);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    complete_and_poll(1);
    TEST_ASSERT_EQUAL_UINT32(1999, tmx_adapter_poll(&fx_a, 1));
}

TEST_CASE("poll with no sensors and nothing to scan: UINT32_MAX", "[adapter]")
{
    fx_start();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 0));
}

TEST_CASE("STOP_ALL_REPORTS: no sensor read starts until reports are enabled again", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND0(TMX_CMD_STOP_ALL_REPORTS, 0);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, tmx_adapter_poll(&fx_a, 0));
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(0, tmx_fake_count("sonar_start"));
    FX_SEND0(TMX_CMD_ENABLE_ALL_REPORTS, 100);
    tmx_adapter_poll(&fx_a, 100);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
}

TEST_CASE("RESET with a sonar and a DHT in progress: devices gone, pins and RMT RX released, late completions dropped",
          "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    tmx_fake_dht_frame(14, frame_ok, ESP_OK);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
    FX_SEND0(TMX_CMD_RESET, 1);
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 12));
    TEST_ASSERT_EQUAL(TMX_USE_NONE, tmx_pin_use(&fx_a.claims, 14));
    TEST_ASSERT_EQUAL(0, fx_a.claims.rmt_rx_used);
    complete_and_poll(2);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
    tmx_adapter_poll(&fx_a, 3000);
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
    TEST_ASSERT_EQUAL(1, tmx_fake_count("dht_start"));
}

TEST_CASE("A device registered after RESET gets the pings of its own generation", "[adapter]")
{
    fx_start();
    tmx_fake_sonar_echo(12, 5800);
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    FX_SEND0(TMX_CMD_RESET, 1);
    FX_SEND(TMX_CMD_SONAR_NEW, 2, 12, 13);
    tmx_adapter_poll(&fx_a, 2);                 /* a new ping starts: the old one is forgotten */
    TEST_ASSERT_EQUAL(2, tmx_fake_count("sonar_start"));
    complete_and_poll(3);                       /* completes the old and the new ping */
    TEST_ASSERT_EQUAL(1, fx_n_reports);
}

TEST_CASE("A completion with an unknown kind or token: dropped without a report", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_completion_t junk[] = {
        {.kind = 0, .token = 0}, {.kind = TMX_COMPLETION_SONAR, .token = 0xffff, .value = 5800},
        {.kind = TMX_COMPLETION_DHT, .token = 0, .err = ESP_OK},
    };
    for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
        tmx_completion_post(&junk[i]);
    }
    tmx_adapter_poll(&fx_a, 0);
    TEST_ASSERT_EQUAL(0, fx_n_reports);
}

TEST_CASE("A sonar ping whose completion never arrives: the sonar recovers after about a second", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_SONAR_NEW, 0, 12, 13);
    tmx_adapter_poll(&fx_a, 0);
    for (uint32_t t = 33; t <= 33 * 29; t += 33) {
        tmx_adapter_poll(&fx_a, t);
    }
    TEST_ASSERT_EQUAL(1, tmx_fake_count("sonar_start"));
    tmx_adapter_poll(&fx_a, 33 * 30);
    tmx_adapter_poll(&fx_a, 33 * 31);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("sonar_start"));
}

TEST_CASE("A DHT read whose completion never arrives: the next 2000 ms pass starts over", "[adapter]")
{
    fx_start();
    FX_SEND(TMX_CMD_DHT_NEW, 0, 14);
    tmx_adapter_poll(&fx_a, 0);
    tmx_adapter_poll(&fx_a, 2000);
    TEST_ASSERT_EQUAL(2, tmx_fake_count("dht_start"));
}
