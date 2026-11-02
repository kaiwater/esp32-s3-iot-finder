#include "wnm_clock.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "wnm_clock";

typedef enum {
    RULE_M = 0,
    RULE_J,
    RULE_N,
} rule_type_t;

typedef struct {
    rule_type_t type;
    int month;
    int week;
    int wday;
    int day;
    int time_sec;
} tz_rule_t;

typedef struct {
    bool valid;
    char std_name[8];
    char dst_name[8];
    int32_t std_off; /* local = UTC + offset */
    int32_t dst_off;
    bool has_dst;
    bool has_rules;
    tz_rule_t start;
    tz_rule_t end;
} tz_t;

static bool s_synced;
static int64_t s_sync_utc;
static uint32_t s_sync_ms;
static bool s_have_source;
static uint8_t s_source[6];

static bool s_has_tz;
static tz_t s_tz;
static char s_tz_text[40];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *yp, unsigned *mp, unsigned *dp)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mprime = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mprime + 2u) / 5u + 1u;
    const unsigned m = (unsigned)((int)mprime + (mprime < 10 ? 3 : -9));
    *yp = y + (m <= 2);
    *mp = m;
    *dp = d;
}

static bool is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 0;
    if (m == 2 && is_leap(y)) return 29;
    return dim[m - 1];
}

static int weekday_from_days(int64_t z)
{
    int w = (int)((z + 4) % 7);
    return w < 0 ? w + 7 : w;
}

static bool parse_name(const char **pp, char *out, size_t n)
{
    const char *p = *pp;
    size_t j = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>' && j + 1 < n) out[j++] = *p++;
        if (*p != '>') return false;
        p++;
    } else {
        while (isalpha((unsigned char)*p) && j + 1 < n) out[j++] = *p++;
    }
    if (!j) return false;
    out[j] = 0;
    *pp = p;
    return true;
}

static bool parse_int(const char **pp, int *out, int max_digits)
{
    const char *p = *pp;
    int v = 0;
    int n = 0;
    while (isdigit((unsigned char)*p) && n < max_digits) {
        v = v * 10 + (*p - '0');
        p++;
        n++;
    }
    if (!n) return false;
    *out = v;
    *pp = p;
    return true;
}

static bool parse_offset(const char **pp, int32_t *out)
{
    const char *p = *pp;
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        p++;
    }

    int h = 0, m = 0, s = 0;
    if (!parse_int(&p, &h, 3)) return false;
    if (*p == ':') {
        p++;
        if (!parse_int(&p, &m, 2)) return false;
        if (*p == ':') {
            p++;
            if (!parse_int(&p, &s, 2)) return false;
        }
    }
    if (h > 24 || m > 59 || s > 59) return false;
    *out = sign * (h * 3600 + m * 60 + s);
    *pp = p;
    return true;
}

static bool parse_rule_time(const char **pp, int *out)
{
    const char *p = *pp;
    int h = 0, m = 0, s = 0;
    if (!parse_int(&p, &h, 2)) return false;
    if (*p == ':') {
        p++;
        if (!parse_int(&p, &m, 2)) return false;
        if (*p == ':') {
            p++;
            if (!parse_int(&p, &s, 2)) return false;
        }
    }
    *out = h * 3600 + m * 60 + s;
    *pp = p;
    return true;
}

static bool parse_rule(const char **pp, tz_rule_t *r)
{
    const char *p = *pp;
    memset(r, 0, sizeof(*r));
    r->time_sec = 2 * 3600;

    if (*p == 'M') {
        p++;
        r->type = RULE_M;
        if (!parse_int(&p, &r->month, 2) || *p++ != '.') return false;
        if (!parse_int(&p, &r->week, 1) || *p++ != '.') return false;
        if (!parse_int(&p, &r->wday, 1)) return false;
        if (r->month < 1 || r->month > 12 || r->week < 1 || r->week > 5 ||
            r->wday > 6) return false;
    } else if (*p == 'J') {
        p++;
        r->type = RULE_J;
        if (!parse_int(&p, &r->day, 3)) return false;
        if (r->day < 1 || r->day > 365) return false;
    } else if (isdigit((unsigned char)*p)) {
        r->type = RULE_N;
        if (!parse_int(&p, &r->day, 3)) return false;
        if (r->day > 365) return false;
    } else {
        return false;
    }

    if (*p == '/') {
        p++;
        if (!parse_rule_time(&p, &r->time_sec)) return false;
    }
    *pp = p;
    return true;
}

static bool parse_tz(const char *text, tz_t *tz)
{
    memset(tz, 0, sizeof(*tz));
    const char *p = text;

    if (!parse_name(&p, tz->std_name, sizeof(tz->std_name))) return false;
    int32_t posix_off;
    if (!parse_offset(&p, &posix_off)) return false;
    tz->std_off = -posix_off; /* POSIX offset is added to local to get UTC */
    tz->dst_off = tz->std_off + 3600;

    if (*p && *p != ',') {
        if (!parse_name(&p, tz->dst_name, sizeof(tz->dst_name))) return false;
        tz->has_dst = true;
        if (*p && *p != ',') {
            if (!parse_offset(&p, &posix_off)) return false;
            tz->dst_off = -posix_off;
        }
        if (*p == ',') {
            p++;
            if (!parse_rule(&p, &tz->start)) return false;
            if (*p++ != ',') return false;
            if (!parse_rule(&p, &tz->end)) return false;
            tz->has_rules = true;
        }
    }
    if (*p) return false;

    tz->valid = true;
    return true;
}

static int rule_day(int year, int month, int week, int wday)
{
    int first = weekday_from_days(days_from_civil(year, (unsigned)month, 1));
    int day = 1 + ((wday - first + 7) % 7) + (week - 1) * 7;
    int dim = days_in_month(year, month);
    while (day > dim) day -= 7;
    if (day < 1) day = 1;
    return day;
}

static int64_t rule_epoch(int year, const tz_rule_t *r, int32_t local_off)
{
    int month = 1;
    int day = 1;

    if (r->type == RULE_M) {
        month = r->month;
        day = rule_day(year, month, r->week, r->wday);
    } else {
        int doy = r->day;
        if (r->type == RULE_J && is_leap(year) && doy >= 60) doy++;
        if (r->type == RULE_N) doy++;
        if (doy < 1) doy = 1;
        if (doy > 366) doy = 366;
        while (month < 12 && doy > days_in_month(year, month)) {
            doy -= days_in_month(year, month);
            month++;
        }
        day = doy;
    }

    int64_t local = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
                    r->time_sec;
    return local - local_off;
}

static bool dst_active(int64_t utc, const tz_t *tz)
{
    if (!tz->has_dst || !tz->has_rules) return false;

    int year;
    unsigned month, day;
    civil_from_days(utc / 86400, &year, &month, &day);
    (void)month;
    (void)day;

    int64_t start = rule_epoch(year, &tz->start, tz->std_off);
    int64_t end = rule_epoch(year, &tz->end, tz->dst_off);
    if (start < end) return utc >= start && utc < end;
    return utc >= start || utc < end;
}

static int32_t tz_offset_at(int64_t utc, const tz_t *tz, const char **name)
{
    if (tz->has_dst && dst_active(utc, tz)) {
        *name = tz->dst_name[0] ? tz->dst_name : "DST";
        return tz->dst_off;
    }
    *name = tz->std_name[0] ? tz->std_name : "UTC";
    return tz->std_off;
}

static bool decode_time_value(const uint8_t tv[10], int64_t *out)
{
    int year = tv[0] | ((int)tv[1] << 8);
    int mon = tv[2];
    int day = tv[3];
    int hour = tv[4];
    int min = tv[5];
    int sec = tv[6];
    int ms = tv[7] | ((int)tv[8] << 8);

    if (year < 2000 || year > 2100) return false;
    if (mon < 1 || mon > 12) return false;
    if (day < 1 || day > days_in_month(year, mon)) return false;
    if (hour > 23 || min > 59 || sec > 59 || ms > 999) return false;

    *out = days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400 +
           hour * 3600 + min * 60 + sec + ms / 1000;
    return true;
}

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

void wnm_clock_init(void)
{
    s_synced = false;
    s_sync_utc = 0;
    s_sync_ms = 0;
    s_have_source = false;
    memset(s_source, 0, sizeof(s_source));
    s_has_tz = false;
    memset(&s_tz, 0, sizeof(s_tz));
    s_tz_text[0] = 0;
}

void wnm_clock_set_utc(const uint8_t time_value[10], uint64_t tsf_us,
                       const uint8_t source[6], uint32_t mono_ms)
{
    int64_t base;
    if (!decode_time_value(time_value, &base)) return;

    int64_t candidate = base + (int64_t)(tsf_us / 1000000ull);
    bool same_source = s_have_source && memcmp(source, s_source, 6) == 0;

    if (s_synced) {
        int64_t est = s_sync_utc +
                      (int64_t)((uint32_t)(mono_ms - s_sync_ms) / 1000u);
        int64_t diff = candidate - est;
        if (!same_source && abs64(diff) <= 5) return;
    }

    bool first = !s_synced;
    bool source_changed = !same_source;
    int64_t old = s_sync_utc;

    s_sync_utc = candidate;
    s_sync_ms = mono_ms;
    s_synced = true;
    memcpy(s_source, source, 6);
    s_have_source = true;

    if (first || source_changed || abs64(candidate - old) > 5) {
        int y;
        unsigned m, d;
        civil_from_days(candidate / 86400, &y, &m, &d);
        int sod = (int)(candidate % 86400);
        if (sod < 0) sod += 86400;
        ESP_LOGI(TAG, "UTC %04d-%02u-%02u %02u:%02u:%02u from %02X:%02X:%02X:%02X:%02X:%02X",
                 y, m, d, sod / 3600, (sod / 60) % 60, sod % 60,
                 source[0], source[1], source[2], source[3], source[4], source[5]);
    }
}

void wnm_clock_set_tz(const uint8_t *tz, uint8_t len, const uint8_t source[6])
{
    static const uint8_t zero_source[6] = {0};
    if (!source) source = zero_source;
    if (!tz || !len) return;
    if (len >= sizeof(s_tz_text)) len = sizeof(s_tz_text) - 1;

    char text[sizeof(s_tz_text)];
    memcpy(text, tz, len);
    text[len] = 0;
    if (!text[0]) return;
    if (s_has_tz && strcmp(text, s_tz_text) == 0) return;

    tz_t parsed;
    if (!parse_tz(text, &parsed)) {
        ESP_LOGD(TAG, "cannot parse TZ '%s'", text);
        return;
    }

    s_tz = parsed;
    s_has_tz = true;
    strncpy(s_tz_text, text, sizeof(s_tz_text) - 1);
    s_tz_text[sizeof(s_tz_text) - 1] = 0;
    ESP_LOGI(TAG, "time zone '%s' offset %+ld min from %02X:%02X:%02X:%02X:%02X:%02X",
             text, (long)(parsed.std_off / 60),
             source[0], source[1], source[2], source[3], source[4], source[5]);
}

void wnm_clock_poll(wnm_clock_view_t *v)
{
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->has_tz = s_has_tz;

    if (!s_synced) {
        if (s_has_tz && s_tz.std_name[0]) {
            strncpy(v->tz_name, s_tz.std_name, sizeof(v->tz_name) - 1);
        }
        return;
    }

    uint32_t now = now_ms();
    int64_t utc = s_sync_utc +
                  (int64_t)((uint32_t)(now - s_sync_ms) / 1000u);
    int32_t off = 0;
    const char *name = "UTC";
    if (s_has_tz) off = tz_offset_at(utc, &s_tz, &name);

    int64_t local = utc + off;
    int y;
    unsigned m, d;
    civil_from_days(local / 86400, &y, &m, &d);
    int sod = (int)(local % 86400);
    if (sod < 0) sod += 86400;

    v->synced = true;
    v->year = (uint16_t)y;
    v->month = (uint8_t)m;
    v->day = (uint8_t)d;
    v->hour = (uint8_t)(sod / 3600);
    v->min = (uint8_t)((sod / 60) % 60);
    v->sec = (uint8_t)(sod % 60);
    v->offset_sec = off;
    strncpy(v->tz_name, name, sizeof(v->tz_name) - 1);
    v->tz_name[sizeof(v->tz_name) - 1] = 0;
    v->age_ms = now - s_sync_ms;
    v->have_source = s_have_source;
    memcpy(v->source, s_source, 6);
}

bool wnm_clock_synced(void)
{
    return s_synced;
}
