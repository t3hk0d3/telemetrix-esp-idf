#include "tmx_core.h"

/* Signed 32-bit difference: positive while the deadline is still ahead, valid for spans under 2^31 ms. */
static int32_t ahead_ms(const tmx_deadline_t *d, uint32_t now_ms)
{
    return (int32_t)(d->due_ms - now_ms);
}

void tmx_deadline_arm(tmx_deadline_t *d, uint32_t now_ms, uint32_t in_ms)
{
    d->due_ms = now_ms + in_ms;
    d->armed = true;
}

bool tmx_deadline_due(const tmx_deadline_t *d, uint32_t now_ms)
{
    return d->armed && ahead_ms(d, now_ms) <= 0;
}

uint32_t tmx_deadline_left(const tmx_deadline_t *d, uint32_t now_ms)
{
    if (!d->armed) {
        return UINT32_MAX;
    }
    int32_t ahead = ahead_ms(d, now_ms);
    return ahead > 0 ? (uint32_t)ahead : 0;
}
