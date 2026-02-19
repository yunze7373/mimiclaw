#include "skills/skill_rate_limit.h"
#include "skills/skill_engine.h"
#include "esp_timer.h"
#include <string.h>

/**
 * Token bucket rate limiter per skill per operation type.
 *
 * Default limits (ops per second):
 *   GPIO: 200 ops/s
 *   I2C:  100 ops/s
 *   HTTP:   2 ops/s  (external network)
 *   UART: 100 ops/s
 */

typedef struct {
    int64_t  last_refill_us;     /* Last refill timestamp */
    float    tokens;             /* Current available tokens */
    float    max_tokens;         /* Bucket capacity */
    float    refill_rate;        /* Tokens per second */
} bucket_t;

/* Default rates per operation type */
static const float s_default_rates[RATE_LIMIT_MAX] = {
    [RATE_LIMIT_GPIO] = 200.0f,
    [RATE_LIMIT_I2C]  = 100.0f,
    [RATE_LIMIT_HTTP] =   2.0f,
    [RATE_LIMIT_UART] = 100.0f,
};

static const float s_default_burst[RATE_LIMIT_MAX] = {
    [RATE_LIMIT_GPIO] = 50.0f,
    [RATE_LIMIT_I2C]  = 20.0f,
    [RATE_LIMIT_HTTP] =  5.0f,
    [RATE_LIMIT_UART] = 20.0f,
};

static bucket_t s_buckets[SKILL_MAX_SLOTS][RATE_LIMIT_MAX];

/* ── Public API ──────────────────────────────────────────────────── */

void skill_rate_limit_init(int skill_id)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS) return;

    int64_t now = esp_timer_get_time();
    for (int t = 0; t < RATE_LIMIT_MAX; t++) {
        bucket_t *b = &s_buckets[skill_id][t];
        b->last_refill_us = now;
        b->max_tokens = s_default_burst[t];
        b->tokens = s_default_burst[t];
        b->refill_rate = s_default_rates[t];
    }
}

bool skill_rate_limit_check(int skill_id, rate_limit_type_t type)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS) return false;
    if (type < 0 || type >= RATE_LIMIT_MAX) return false;

    bucket_t *b = &s_buckets[skill_id][type];

    /* Refill tokens based on elapsed time */
    int64_t now = esp_timer_get_time();
    float elapsed_s = (now - b->last_refill_us) / 1000000.0f;
    b->last_refill_us = now;

    b->tokens += elapsed_s * b->refill_rate;
    if (b->tokens > b->max_tokens) {
        b->tokens = b->max_tokens;
    }

    /* Try to consume one token */
    if (b->tokens >= 1.0f) {
        b->tokens -= 1.0f;
        return true;
    }

    return false;  /* Rate exceeded */
}

void skill_rate_limit_reset(int skill_id)
{
    if (skill_id < 0 || skill_id >= SKILL_MAX_SLOTS) return;
    memset(&s_buckets[skill_id], 0, sizeof(s_buckets[skill_id]));
}
