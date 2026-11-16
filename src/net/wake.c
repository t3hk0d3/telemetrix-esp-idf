#include <unistd.h>

#include "tmx_net.h"

#if CONFIG_IDF_TARGET_LINUX
#include <sys/eventfd.h>
#else
#include "esp_vfs_eventfd.h"
#endif

static volatile int wake_fd = -1;

esp_err_t tmx_wake_open(void)
{
    if (wake_fd >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_IDF_TARGET_LINUX
    int fd = eventfd(0, EFD_NONBLOCK);
#else
    esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already registered by the application */
        return err;
    }
    int fd = eventfd(0, EFD_SUPPORT_ISR);
#endif
    if (fd < 0) {
        return ESP_ERR_NO_MEM;
    }
    wake_fd = fd;
    return ESP_OK;
}

int tmx_wake_fd(void)
{
    return wake_fd;
}

void tmx_wake_drain(void)
{
    int fd = wake_fd;
    if (fd < 0) {
        return;
    }
    uint64_t count;
    /* only called after select() said readable, so the read does not block; the linux eventfd is nonblocking anyway */
    ssize_t r = read(fd, &count, sizeof(count));
    (void)r;
}

void tmx_wake_close(void)
{
    int fd = wake_fd;
    wake_fd = -1;                       /* a late tmx_net_wake sees -1 */
    if (fd >= 0) {
        close(fd);
    }
}

/* Callable from an ISR on chips: a write to an eventfd opened with EFD_SUPPORT_ISR is ISR-safe. */
void tmx_net_wake(void)
{
    int fd = wake_fd;
    if (fd < 0) {
        return;
    }
    const uint64_t one = 1;
    ssize_t r = write(fd, &one, sizeof(one));
    (void)r;
}
