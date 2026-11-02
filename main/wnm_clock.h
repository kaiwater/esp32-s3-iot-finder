#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool synced;
    bool has_tz;
    char tz_name[8];
    int32_t offset_sec;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint32_t age_ms;
    bool have_source;
    uint8_t source[6];
    bool tsf_mode;
    bool persisted;
} wnm_clock_view_t;

void wnm_clock_init(void);

/* Time Advertisement (element 69, timing capability 2):
 * the 10-byte value is UTC broken-down time at which the AP TSF was zero.
 * Adding the beacon TSF gives current UTC. */
void wnm_clock_set_utc(const uint8_t time_value[10], uint64_t tsf_us,
                       const uint8_t source[6], uint32_t mono_ms);

/* Time Zone element (98): POSIX TZ string, e.g. CST-8 or
 * CET-1CEST,M3.5.0,M10.5.0/3. */
void wnm_clock_set_tz(const uint8_t *tz, uint8_t len, const uint8_t source[6]);
void wnm_clock_set_mode(const char *mode);
void wnm_clock_clear_anchor(void);

void wnm_clock_poll(wnm_clock_view_t *v);
bool wnm_clock_synced(void);
