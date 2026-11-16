#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fake/tmx_drv_fake.h"
#include "host_profile.h"
#include "test_plugin.h"
#include "telemetrix.h"

#define MAX_ARGS 32
#define MAX_ALLOWED 8
#define COMPLETE_MS 5

/* SIGUSR1 belongs to the FreeRTOS linux port (task resume); SIGUSR2 prints the counters. */
static volatile sig_atomic_t stop_requested, status_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void on_status(int sig)
{
    (void)sig;
    status_requested = 1;
}

static void usage(const char *why)
{
    fprintf(stderr, "%s\nusage: host_server --port N [--allow CIDR]... [--stall-ms N] [--stock-only] [--test-plugin]\n", why);
    exit(2);
}

/** The program's arguments: the IDF linux target enters through app_main, so they come from /proc. */
static int read_args(char *buf, size_t size, char **argv)
{
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) {
        usage("cannot read /proc/self/cmdline");
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        usage("cannot read /proc/self/cmdline");
    }
    buf[n] = '\0';
    int argc = 0;
    for (char *p = buf; p < buf + n && argc < MAX_ARGS; p += strlen(p) + 1) {
        argv[argc++] = p;
    }
    return argc;
}

static unsigned long number(const char *text, unsigned long max)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(text, &end, 10);
    if (errno || end == text || *end || v > max) {
        usage("bad number");
    }
    return v;
}

/** Stand in for the hardware: DHT and sonar starts of the fake complete only when asked to. */
static void complete_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(COMPLETE_MS));
        tmx_fake_complete_pending();
    }
}

static void print_status(void)
{
    telemetrix_status_t st;
    if (telemetrix_get_status(&st) == ESP_OK) {
        printf("status client=%s refused_connections=%u dropped_packets=%u ignored_commands=%u refused_commands=%u "
               "coalesced_reports=%u stalled_clients=%u ledc_attach=%u ledc_duty=%u extended=%d protocol_violations=%u "
               "error_reports=%u\n", st.client[0] ? st.client : "-",
               (unsigned)st.refused_connections, (unsigned)st.dropped_packets, (unsigned)st.ignored_commands,
               (unsigned)st.refused_commands, (unsigned)st.coalesced_reports, (unsigned)st.stalled_clients, (unsigned)tmx_fake_count("ledc_attach"),
               (unsigned)tmx_fake_count("ledc_duty"), st.extended ? 1 : 0, (unsigned)st.protocol_violations,
               (unsigned)st.error_reports);
        fflush(stdout);
    }
}

void app_main(void)
{
    static char buf[4096];
    char *argv[MAX_ARGS];
    int argc = read_args(buf, sizeof(buf), argv);
    telemetrix_config_t cfg;
    telemetrix_cidr_t allowed[MAX_ALLOWED];
    size_t n_allowed = 0;
    long port = -1;
    bool test_plugin = false;
    static const telemetrix_plugin_t *const plugins[] = {&test_plugin_echo};

    telemetrix_config_default(&cfg);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stock-only")) {
            cfg.extended = false;
            continue;
        }
        if (!strcmp(argv[i], "--test-plugin")) {
            test_plugin = true;
            continue;
        }
        if (i + 1 >= argc) {
            usage("missing value");
        }
        if (!strcmp(argv[i], "--port")) {
            port = (long)number(argv[++i], 65535);
        } else if (!strcmp(argv[i], "--stall-ms")) {
            cfg.send_stall_ms = (uint16_t)number(argv[++i], 65535);
        } else if (!strcmp(argv[i], "--allow")) {
            if (n_allowed >= MAX_ALLOWED || telemetrix_cidr_parse(argv[++i], &allowed[n_allowed]) != ESP_OK) {
                usage("bad --allow");
            }
            n_allowed++;
        } else {
            usage("unknown argument");
        }
    }
    if (port <= 0) {
        usage("--port is required and must not be 0");
    }
    cfg.port = (uint16_t)port;
    cfg.allowed = allowed;
    cfg.n_allowed = n_allowed;
    if (test_plugin) {
        cfg.plugins = plugins;
        cfg.n_plugins = 1;
    }

    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    signal(SIGUSR2, on_status);

    host_script_fake();
    if (telemetrix_start(host_profile(test_plugin), &cfg, NULL) != ESP_OK) {
        fprintf(stderr, "telemetrix_start failed\n");
        exit(1);
    }
    xTaskCreate(complete_task, "fake_complete", 4096, NULL, 4, NULL);
    printf("listening %u\n", (unsigned)cfg.port);
    fflush(stdout);

    while (!stop_requested) {
        if (status_requested) {
            status_requested = 0;
            print_status();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    telemetrix_stop();
    exit(0);
}
