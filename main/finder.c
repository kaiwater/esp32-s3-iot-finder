#include "finder.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "oui.h"
#include "wnm_clock.h"

#define EVT_QUEUE_LEN 96
#define CAND_TABLE_LEN 32
#define BSS_TABLE_LEN 32

#define HOP_MS 150
#define LEARN_MS 5000
#define ACTIVE_MS 60000
#define TOAST_MS 2600

#define CH_MIN 1
#define CH_MAX 13

static const char *TAG = "finder";

typedef struct {
    bool valid;
    uint16_t stations;
    uint8_t utilization;
    uint16_t admission;
} bss_load_t;

typedef struct {
    uint32_t ms;
    int8_t rssi;
    uint8_t ch;
    uint8_t type;
    uint8_t subtype;
    uint8_t tods;
    uint8_t fromds;
    uint8_t ssid_len;
    uint8_t ds_ch;
    uint8_t ssid[FINDER_SSID_MAX];

    bss_load_t load;
    uint8_t time_capab;
    uint8_t time_value_len;
    uint8_t time_value[10];
    uint8_t tz_len;
    uint8_t tz[40];
    bool has_tsf;
    uint64_t tsf;

    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    uint8_t addr4[6];
} snif_evt_t;

static QueueHandle_t s_evt_q;
static SemaphoreHandle_t s_api_lock;
static SemaphoreHandle_t s_tab_lock;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_toast_mux = portMUX_INITIALIZER_UNLOCKED;

static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static bool s_wifi_ok;

static uint8_t s_target[6];
static volatile bool s_target_set;

static volatile uint32_t s_t_frames;
static volatile uint32_t s_t_last_ms;
static volatile int8_t s_t_rssi;
static volatile uint8_t s_t_channel;
static volatile uint8_t s_t_ftype;
static uint8_t s_t_bssid[6];
static volatile bool s_t_have_bssid;
static int8_t s_rssi_hist[5];
static volatile uint8_t s_rssi_n;
static volatile uint8_t s_rssi_i;

static int8_t s_peak = -128;
static char s_ssid_hint[FINDER_SSID_MAX];
static uint8_t s_ssid_hint_len;

/* Diagnostic counters for 802.11 WNM time elements. */
static volatile uint32_t s_ie67_count;
static volatile uint32_t s_ie69_count;
static volatile uint32_t s_ie98_count;
static uint8_t s_last_ie69[11];
static uint8_t s_last_ie69_len;
static char s_last_ie98[40];

static volatile uint32_t s_learn_until;
static uint32_t s_packets;
static volatile uint32_t s_drops;

static uint8_t s_cur_ch = CH_MIN;
static uint8_t s_hop_ch = CH_MIN;
static uint8_t s_manual_ch = CH_MIN;
static uint8_t s_target_ch;
static bool s_auto_lock = true;
static finder_chan_mode_t s_mode = FINDER_CH_AUTO;
static uint32_t s_last_hop_ms;

static uint32_t s_fps_sample_ms;
static uint32_t s_fps_last_frames;
static uint32_t s_fps_hist[4];
static uint8_t s_fps_i;
static uint8_t s_fps_n;

static char s_toast[FINDER_TOAST_MAX];
static uint32_t s_toast_until;

typedef struct {
    bool used;
    uint8_t mac[6];
    uint8_t bssid[6];
    int8_t rssi;
    int8_t rssi_max;
    uint8_t channel;
    uint32_t frames;
    uint32_t last_ms;
    uint32_t learn_frames;
    int8_t learn_rssi;
    bool local;
    char probe_ssid[FINDER_SSID_MAX];
    uint8_t probe_ssid_len;
} cand_rec_t;

typedef struct {
    bool used;
    uint8_t bssid[6];
    char ssid[FINDER_SSID_MAX];
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;
    uint32_t last_ms;
    bool has_load;
    uint16_t load_sta;
    uint8_t load_util;
    uint16_t load_aac;
} bss_rec_t;

static cand_rec_t s_cand[CAND_TABLE_LEN];
static bss_rec_t s_bss[BSS_TABLE_LEN];

typedef struct {
    uint8_t mac[6];
    uint32_t last_ms;
    int8_t rssi;
    uint8_t ch;
} pick_t;

typedef struct {
    uint8_t mac[6];
    uint8_t bssid[6];
    uint32_t last_ms;
    uint32_t frames;
    int8_t rssi;
    uint8_t ch;
    char ap_ssid[FINDER_SSID_MAX];
    uint8_t ap_ssid_len;
    bool ap_known;
    bool ap_hidden;
    char probe_ssid[FINDER_SSID_MAX];
    uint8_t probe_ssid_len;
} dump_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[FINDER_SSID_MAX];
    uint8_t ssid_len;
    uint8_t ch;
    int8_t rssi;
    uint32_t last_ms;
    bool has_load;
    uint16_t load_sta;
    uint8_t load_util;
    uint16_t load_aac;
} bss_dump_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool mac_group(const uint8_t *m) { return (m[0] & 1) != 0; }

static bool mac_zero(const uint8_t *m)
{
    for (int i = 0; i < 6; i++)
        if (m[i]) return false;
    return true;
}

static bool mac_valid(const uint8_t *m)
{
    return !mac_zero(m) && !mac_group(m);
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void mac_text(const uint8_t *m, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void parse_ies(const uint8_t *p, int len, uint8_t *ssid,
                      uint8_t *ssid_len, uint8_t *ds_ch, bss_load_t *load,
                      uint8_t *time_capab, uint8_t *time_value,
                      uint8_t *time_value_len, uint8_t *tz, uint8_t *tz_len)
{
    *ssid_len = 0;
    *ds_ch = 0;
    *time_value_len = 0;
    *tz_len = 0;
    while (len >= 2) {
        int id = p[0];
        int l = p[1];
        if (l + 2 > len) break;
        if (id == 0 && l <= 32) {
            memcpy(ssid, p + 2, (size_t)l);
            *ssid_len = (uint8_t)l;
        } else if (id == 3 && l >= 1) {
            *ds_ch = p[2];
        } else if (id == 11 && l >= 5) {
            load->valid = true;
            load->stations = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
            load->utilization = p[4];
            load->admission = (uint16_t)(p[5] | ((uint16_t)p[6] << 8));
        } else if (id == 67) {
            s_ie67_count++;
        } else if (id == 69 && l >= 11) {
            /* Time Advertisement.  The 10-byte value follows the timing
             * capabilities byte. */
            s_ie69_count++;
            *time_capab = p[2];
            memcpy(time_value, p + 3, 10);
            *time_value_len = 10;
            s_last_ie69[0] = p[2];
            memcpy(&s_last_ie69[1], p + 3, 10);
            s_last_ie69_len = 11;
        } else if (id == 98 && l > 0 && l < 40) {
            s_ie98_count++;
            memcpy(tz, p + 2, (size_t)l);
            tz[l] = 0;
            *tz_len = (uint8_t)l;
            memcpy(s_last_ie98, p + 2, (size_t)l);
            s_last_ie98[l] = 0;
        }
        p += 2 + l;
        len -= 2 + l;
    }
}

static void set_channel(uint8_t ch)
{
    if (ch < CH_MIN) ch = CH_MIN;
    if (ch > CH_MAX) ch = CH_MAX;
    if (ch == s_cur_ch) return;

    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        s_cur_ch = ch;
    } else {
        ESP_LOGD(TAG, "set channel %u: %s", ch, esp_err_to_name(err));
    }
}

static void reset_target_stats(void)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_t_frames = 0;
    s_t_last_ms = 0;
    s_t_rssi = -100;
    s_t_channel = 0;
    s_t_ftype = 0;
    s_t_have_bssid = false;
    memset(s_t_bssid, 0, sizeof(s_t_bssid));
    memset(s_rssi_hist, 0, sizeof(s_rssi_hist));
    s_rssi_n = 0;
    s_rssi_i = 0;
    portEXIT_CRITICAL(&s_stats_mux);

    s_peak = -128;
    s_ssid_hint_len = 0;
    s_ssid_hint[0] = 0;

    s_fps_sample_ms = now_ms();
    s_fps_last_frames = 0;
    memset(s_fps_hist, 0, sizeof(s_fps_hist));
    s_fps_i = 0;
    s_fps_n = 0;
}

static int8_t rssi_median(void)
{
    int8_t v[5];
    uint8_t n;
    portENTER_CRITICAL(&s_stats_mux);
    n = s_rssi_n;
    memcpy(v, s_rssi_hist, sizeof(v));
    portEXIT_CRITICAL(&s_stats_mux);

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (v[j] < v[i]) {
                int8_t t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    if (!n) return -100;
    return v[n / 2];
}

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 10) return;

    uint16_t fc = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
    uint8_t ftype = (uint8_t)((fc >> 2) & 3);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0x0F);
    bool tods = (fc & 0x0100) != 0;
    bool fromds = (fc & 0x0200) != 0;
    uint32_t now = now_ms();

    uint8_t a1[6] = {0}, a2[6] = {0}, a3[6] = {0}, a4[6] = {0};
    memcpy(a1, f + 4, 6);
    if (len >= 16) memcpy(a2, f + 10, 6);
    if (len >= 24) memcpy(a3, f + 16, 6);
    if (ftype == 2 && tods && fromds && len >= 30) memcpy(a4, f + 24, 6);

    bool match = false;
    if (s_target_set) {
        match = mac_eq(a1, s_target) ||
                (len >= 16 && mac_eq(a2, s_target)) ||
                (len >= 24 && mac_eq(a3, s_target)) ||
                (len >= 30 && mac_eq(a4, s_target));
    }

    if (match) {
        const uint8_t *bss = NULL;
        if (ftype == 0 && len >= 24) {
            bss = a3;
        } else if (ftype == 2) {
            if (tods && !fromds) bss = a1;
            else if (!tods && fromds) bss = a2;
            else if (!tods && !fromds) bss = a3;
        }

        portENTER_CRITICAL(&s_stats_mux);
        s_t_frames++;
        s_t_last_ms = now;
        s_t_rssi = pkt->rx_ctrl.rssi;
        s_t_channel = pkt->rx_ctrl.channel;
        s_t_ftype = (uint8_t)((ftype << 4) | subtype);
        s_rssi_hist[s_rssi_i] = pkt->rx_ctrl.rssi;
        s_rssi_i = (uint8_t)((s_rssi_i + 1) % 5);
        if (s_rssi_n < 5) s_rssi_n++;
        if (bss && !mac_group(bss) && !mac_zero(bss)) {
            memcpy(s_t_bssid, bss, 6);
            s_t_have_bssid = true;
        }
        portEXIT_CRITICAL(&s_stats_mux);
    }

    /* The Wi-Fi task gets a compact event.  Parsing, candidate learning and
     * display all happen in the main task. */
    if (ftype != 0 && ftype != 2) return;

    snif_evt_t ev = {0};
    ev.ms = now;
    ev.rssi = pkt->rx_ctrl.rssi;
    ev.ch = pkt->rx_ctrl.channel;
    ev.type = ftype;
    ev.subtype = subtype;
    ev.tods = tods ? 1 : 0;
    ev.fromds = fromds ? 1 : 0;
    memcpy(ev.addr1, a1, 6);
    memcpy(ev.addr2, a2, 6);
    memcpy(ev.addr3, a3, 6);
    memcpy(ev.addr4, a4, 6);

    if (ftype == 0 && (subtype == 4 || subtype == 5 || subtype == 8)) {
        int off = (subtype == 4) ? 24 : 36;
        if (len > off) {
            parse_ies(f + off, len - off, ev.ssid, &ev.ssid_len, &ev.ds_ch,
                      &ev.load, &ev.time_capab, ev.time_value,
                      &ev.time_value_len, ev.tz, &ev.tz_len);
        }
        if ((subtype == 5 || subtype == 8) && len >= 36) {
            uint64_t tsf = 0;
            for (int i = 0; i < 8; i++) {
                tsf |= (uint64_t)f[24 + i] << (8 * i);
            }
            ev.tsf = tsf;
            ev.has_tsf = true;
        }
        /* Beacon-flood frames carry no SSID and a DS channel that disagrees
         * with the receive channel.  Do not spend queue space on them. */
        if ((subtype == 5 || subtype == 8) && !ev.ssid_len && ev.ds_ch &&
            ev.ds_ch != ev.ch) {
            return;
        }
    }

    if (xQueueSend(s_evt_q, &ev, 0) != pdTRUE) s_drops++;
}

static bss_rec_t *bss_find(const uint8_t *bssid)
{
    for (int i = 0; i < BSS_TABLE_LEN; i++) {
        if (s_bss[i].used && mac_eq(s_bss[i].bssid, bssid)) return &s_bss[i];
    }
    return NULL;
}

static void bss_update(const snif_evt_t *ev)
{
    if (mac_group(ev->addr3) || mac_zero(ev->addr3)) return;

    /* A beacon flood has no SSID and often a DS channel that does not match
     * the channel it was heard on.  Real hidden APs still carry a DS channel;
     * keep them so a STA's BSSID can be resolved to a known (hidden) AP. */
    if (!ev->ssid_len && !ev->ds_ch && !bss_find(ev->addr3)) return;
    if (ev->ds_ch && ev->ch && ev->ds_ch != ev->ch) return;

    bss_rec_t *r = bss_find(ev->addr3);
    if (!r) {
        int slot = -1;
        for (int i = 0; i < BSS_TABLE_LEN; i++) {
            if (!s_bss[i].used) { slot = i; break; }
        }
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < BSS_TABLE_LEN; i++) {
                if (s_bss[i].last_ms < s_bss[slot].last_ms) slot = i;
            }
        }
        r = &s_bss[slot];
        memset(r, 0, sizeof(*r));
        memcpy(r->bssid, ev->addr3, 6);
        r->used = true;
    }

    if (ev->ssid_len) {
        memcpy(r->ssid, ev->ssid, ev->ssid_len);
        r->ssid[ev->ssid_len] = 0;
        r->ssid_len = ev->ssid_len;
    }
    uint8_t ch = ev->ds_ch ? ev->ds_ch : ev->ch;
    if (ch >= CH_MIN && ch <= 14) r->channel = ch;
    r->rssi = ev->rssi;
    r->last_ms = ev->ms;
    if (ev->load.valid) {
        r->has_load = true;
        r->load_sta = ev->load.stations;
        r->load_util = ev->load.utilization;
        r->load_aac = ev->load.admission;
    }
}

static cand_rec_t *cand_find(const uint8_t *mac)
{
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        cand_rec_t *c = &s_cand[i];
        if (c->used && mac_eq(c->mac, mac)) return c;
    }
    return NULL;
}

static cand_rec_t *cand_get(const uint8_t *mac)
{
    cand_rec_t *c = cand_find(mac);
    if (c) return c;

    uint32_t h = (((uint32_t)mac[3] << 8) | mac[4]) % CAND_TABLE_LEN;
    int slot = -1;
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        int idx = (int)((h + i) % CAND_TABLE_LEN);
        if (!s_cand[idx].used) { slot = idx; break; }
    }
    if (slot < 0) {
        slot = (int)h;
        for (int i = 1; i < CAND_TABLE_LEN; i++) {
            int idx = (int)((h + i) % CAND_TABLE_LEN);
            if (s_cand[idx].last_ms < s_cand[slot].last_ms) slot = idx;
        }
    }

    c = &s_cand[slot];
    memset(c, 0, sizeof(*c));
    memcpy(c->mac, mac, 6);
    c->used = true;
    c->local = (mac[0] & 0x02) != 0;
    c->learn_rssi = -128;
    return c;
}

static void cand_touch(const uint8_t *mac, int8_t rssi, uint8_t ch,
                       const uint8_t *bssid, uint32_t ms, bool learn)
{
    if (mac_group(mac) || mac_zero(mac)) return;

    cand_rec_t *c = cand_get(mac);
    c->frames++;
    c->last_ms = ms;
    c->rssi = rssi;
    c->channel = ch;
    if (rssi > c->rssi_max || c->frames == 1) c->rssi_max = rssi;
    if (bssid && !mac_group(bssid) && !mac_zero(bssid)) memcpy(c->bssid, bssid, 6);

    if (learn) {
        c->learn_frames++;
        if (rssi > c->learn_rssi) c->learn_rssi = rssi;
    }
}

static void cand_note_ssid(const uint8_t *mac, const uint8_t *ssid, uint8_t len)
{
    if (mac_group(mac) || mac_zero(mac)) return;
    if (!len || len >= FINDER_SSID_MAX) return;
    cand_rec_t *c = cand_get(mac);
    memcpy(c->probe_ssid, ssid, len);
    c->probe_ssid[len] = 0;
    c->probe_ssid_len = len;
}

static bool cand_probe_ssid(const uint8_t *mac, char *out, size_t n,
                            uint8_t *out_len)
{
    bool found = false;
    if (xSemaphoreTake(s_tab_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        cand_rec_t *c = cand_find(mac);
        if (c && c->probe_ssid_len && c->probe_ssid_len < n) {
            memcpy(out, c->probe_ssid, c->probe_ssid_len);
            out[c->probe_ssid_len] = 0;
            *out_len = c->probe_ssid_len;
            found = true;
        }
        xSemaphoreGive(s_tab_lock);
    }
    return found;
}

static void drain_events(void)
{
    if (!s_evt_q) return;
    if (xSemaphoreTake(s_tab_lock, pdMS_TO_TICKS(30)) != pdTRUE) return;

    snif_evt_t ev;
    while (xQueueReceive(s_evt_q, &ev, 0) == pdTRUE) {
        s_packets++;
        bool learn = s_learn_until != 0 && ev.ms < s_learn_until;

        if (ev.type == 0) {
            if (ev.subtype == 8 || ev.subtype == 5) {
                bss_update(&ev);
                if (ev.time_value_len == 10 && ev.time_capab == 2 && ev.has_tsf) {
                    wnm_clock_set_utc(ev.time_value, ev.tsf, ev.addr3, ev.ms);
                }
                if (ev.tz_len) {
                    wnm_clock_set_tz(ev.tz, ev.tz_len, ev.addr3);
                }
            }

            if (ev.subtype == 4) {
                if (s_target_set && mac_eq(ev.addr2, s_target) && ev.ssid_len) {
                    memcpy(s_ssid_hint, ev.ssid, ev.ssid_len);
                    s_ssid_hint[ev.ssid_len] = 0;
                    s_ssid_hint_len = ev.ssid_len;
                }
                cand_touch(ev.addr2, ev.rssi, ev.ch, NULL, ev.ms, learn);
                if (ev.ssid_len) cand_note_ssid(ev.addr2, ev.ssid, ev.ssid_len);
            } else if (ev.subtype == 0 || ev.subtype == 2 || ev.subtype == 11) {
                cand_touch(ev.addr2, ev.rssi, ev.ch, ev.addr3, ev.ms, learn);
            }
        } else if (ev.type == 2) {
            if (ev.tods && !ev.fromds) {
                cand_touch(ev.addr2, ev.rssi, ev.ch, ev.addr1, ev.ms, learn);
            } else if (!ev.tods && ev.fromds) {
                cand_touch(ev.addr1, ev.rssi, ev.ch, ev.addr2, ev.ms, learn);
            } else if (!ev.tods && !ev.fromds) {
                cand_touch(ev.addr2, ev.rssi, ev.ch, ev.addr3, ev.ms, learn);
                if (!mac_eq(ev.addr1, ev.addr2) && !mac_eq(ev.addr1, ev.addr3))
                    cand_touch(ev.addr1, ev.rssi, ev.ch, ev.addr3, ev.ms, learn);
            } else {
                cand_touch(ev.addr4, ev.rssi, ev.ch, ev.addr2, ev.ms, learn);
            }
        }
    }

    xSemaphoreGive(s_tab_lock);
}

static void channel_logic(finder_view_t *v, uint32_t now)
{
    if (!s_wifi_ok) return;

    if (s_target_set && s_mode == FINDER_CH_AUTO && s_auto_lock) {
        uint32_t last = s_t_last_ms;
        uint8_t ch = s_t_channel;
        if (last && now - last < 1000 && ch >= CH_MIN && ch <= CH_MAX) {
            s_target_ch = ch;
            s_mode = FINDER_CH_LOCKED;
            set_channel(ch);
        }
    }

    switch (s_mode) {
    case FINDER_CH_LOCKED:
        if (s_target_ch >= CH_MIN && s_target_ch <= CH_MAX) set_channel(s_target_ch);
        break;
    case FINDER_CH_MANUAL:
        set_channel(s_manual_ch);
        break;
    case FINDER_CH_AUTO:
    default:
        if (s_last_hop_ms == 0 || now - s_last_hop_ms >= HOP_MS) {
            s_last_hop_ms = now;
            s_hop_ch = (s_hop_ch >= CH_MAX) ? CH_MIN : (uint8_t)(s_hop_ch + 1);
            set_channel(s_hop_ch);
        }
        break;
    }

    v->channel = s_cur_ch;
    v->target_channel = s_target_ch;
    v->chan_mode = s_mode;
}

static void nvs_load_target(void)
{
    if (!s_nvs_ok) return;
    uint8_t mac[6];
    size_t len = sizeof(mac);
    if (nvs_get_blob(s_nvs, "mac", mac, &len) == ESP_OK && len == sizeof(mac) &&
        mac_valid(mac)) {
        memcpy(s_target, mac, 6);
        s_target_set = true;
        reset_target_stats();
        char text[18];
        mac_text(mac, text);
        ESP_LOGI(TAG, "restored target %s", text);
    }
}

static void nvs_save_target(const uint8_t *mac)
{
    if (!s_nvs_ok) return;
    esp_err_t err = nvs_set_blob(s_nvs, "mac", mac, 6);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "target save failed: %s", esp_err_to_name(err));
}

static void nvs_clear_target(void)
{
    if (!s_nvs_ok) return;
    esp_err_t err = nvs_erase_key(s_nvs, "mac");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "target clear failed: %s", esp_err_to_name(err));
}

void finder_toast(const char *fmt, ...)
{
    char buf[FINDER_TOAST_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    portENTER_CRITICAL(&s_toast_mux);
    strncpy(s_toast, buf, sizeof(s_toast) - 1);
    s_toast[sizeof(s_toast) - 1] = 0;
    s_toast_until = now_ms() + TOAST_MS;
    portEXIT_CRITICAL(&s_toast_mux);
}

bool finder_set_target(const uint8_t mac[6], const char *why)
{
    if (!mac_valid(mac)) {
        finder_toast("INVALID MAC");
        return false;
    }

    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    bool changed = !s_target_set || !mac_eq(s_target, mac);

    s_target_set = false;
    memcpy(s_target, mac, 6);
    s_target_set = true;
    reset_target_stats();

    s_mode = FINDER_CH_AUTO;
    s_auto_lock = true;
    s_target_ch = 0;
    s_learn_until = 0;

    if (changed) nvs_save_target(mac);
    xSemaphoreGive(s_api_lock);

    char text[18];
    mac_text(mac, text);
    ESP_LOGI(TAG, "target set %s (%s)", text, why ? why : "manual");
    finder_toast("TARGET SET");
    return true;
}

bool finder_set_target_str(const char *text)
{
    if (!text) return false;
    char buf[64];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    char *p = buf;
    while (*p && !isxdigit((unsigned char)*p)) p++;
    for (char *q = p; *q; q++) {
        if (*q == '-') *q = ':';
    }

    unsigned v[6];
    if (sscanf(p, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) return false;
        mac[i] = (uint8_t)v[i];
    }
    return finder_set_target(mac, "console");
}

void finder_clear_target(void)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    bool had = s_target_set;
    s_target_set = false;
    memset(s_target, 0, 6);
    reset_target_stats();
    s_mode = FINDER_CH_AUTO;
    s_auto_lock = true;
    s_target_ch = 0;
    s_learn_until = 0;
    nvs_clear_target();
    xSemaphoreGive(s_api_lock);

    ESP_LOGI(TAG, "target cleared");
    finder_toast(had ? "TARGET CLEARED" : "NO TARGET");
}

void finder_start_learn(void)
{
    if (!s_wifi_ok) {
        finder_toast("WIFI NOT READY");
        return;
    }

    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    s_learn_until = now_ms() + LEARN_MS;
    s_mode = FINDER_CH_AUTO;
    s_auto_lock = false;
    s_last_hop_ms = 0;
    xSemaphoreGive(s_api_lock);

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        s_cand[i].learn_frames = 0;
        s_cand[i].learn_rssi = -128;
    }
    xSemaphoreGive(s_tab_lock);

    ESP_LOGI(TAG, "learning for %d ms", LEARN_MS);
    finder_toast("LEARNING 5S");
}

void finder_toggle_lock(void)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (!s_target_set) {
        xSemaphoreGive(s_api_lock);
        finder_toast("SET TARGET FIRST");
        return;
    }

    if (s_mode == FINDER_CH_AUTO) {
        uint8_t ch = s_t_channel;
        if (ch < CH_MIN || ch > CH_MAX) ch = s_cur_ch;
        s_target_ch = ch;
        s_manual_ch = ch;
        s_mode = FINDER_CH_LOCKED;
        s_auto_lock = false;
        set_channel(ch);
        xSemaphoreGive(s_api_lock);
        finder_toast("LOCK CH %u", ch);
    } else {
        s_mode = FINDER_CH_AUTO;
        s_auto_lock = false;
        s_last_hop_ms = 0;
        xSemaphoreGive(s_api_lock);
        finder_toast("CHANNEL HOP");
    }
}

static void step_channel(int dir)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    int ch = (int)s_cur_ch + dir;
    if (ch < CH_MIN) ch = CH_MAX;
    if (ch > CH_MAX) ch = CH_MIN;
    s_manual_ch = (uint8_t)ch;
    s_target_ch = s_manual_ch;
    s_mode = FINDER_CH_MANUAL;
    s_auto_lock = false;
    set_channel(s_manual_ch);
    xSemaphoreGive(s_api_lock);
    finder_toast("MANUAL CH %u", s_manual_ch);
}

void finder_next_channel(void)
{
    step_channel(1);
}

void finder_prev_channel(void)
{
    step_channel(-1);
}

void finder_clear_peak(void)
{
    if (s_target_set && s_t_last_ms) {
        portENTER_CRITICAL(&s_stats_mux);
        s_peak = s_t_rssi;
        portEXIT_CRITICAL(&s_stats_mux);
    } else {
        s_peak = -128;
    }
    finder_toast("PEAK CLEARED");
}

static void target_lock_channel(uint8_t ch)
{
    if (ch < CH_MIN || ch > CH_MAX) return;
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s_target_set) {
        s_target_ch = ch;
        s_manual_ch = ch;
        s_mode = FINDER_CH_LOCKED;
        s_auto_lock = true;
        set_channel(ch);
    }
    xSemaphoreGive(s_api_lock);
}

static void step_candidate(int dir)
{
    pick_t list[CAND_TABLE_LEN];
    int n = 0;
    uint32_t now = now_ms();

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        cand_rec_t *c = &s_cand[i];
        if (!c->used || now - c->last_ms > ACTIVE_MS) continue;
        memcpy(list[n].mac, c->mac, 6);
        list[n].last_ms = c->last_ms;
        list[n].rssi = c->rssi;
        list[n].ch = c->channel;
        n++;
    }
    xSemaphoreGive(s_tab_lock);

    if (!n) {
        finder_toast("NO CANDIDATES");
        return;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (list[j].last_ms > list[i].last_ms) {
                pick_t t = list[i];
                list[i] = list[j];
                list[j] = t;
            }
        }
    }

    int next = dir > 0 ? 0 : n - 1;
    if (s_target_set) {
        for (int i = 0; i < n; i++) {
            if (mac_eq(list[i].mac, s_target)) {
                next = (i + dir + n) % n;
                break;
            }
        }
    }

    finder_set_target(list[next].mac, dir > 0 ? "next candidate" : "prev candidate");
    target_lock_channel(list[next].ch);
}

void finder_next_candidate(void)
{
    step_candidate(1);
}

void finder_prev_candidate(void)
{
    step_candidate(-1);
}

void finder_print_wnm(void)
{
    char hex[24] = "--";
    if (s_last_ie69_len == 11) {
        for (int i = 0; i < 11; i++) {
            snprintf(hex + i * 2, 3, "%02X", s_last_ie69[i]);
        }
    }
    ESP_LOGI(TAG, "WNM IE 67=%lu 69=%lu 98=%lu last69=%s tz='%s'",
             (unsigned long)s_ie67_count, (unsigned long)s_ie69_count,
             (unsigned long)s_ie98_count, hex,
             s_last_ie98[0] ? s_last_ie98 : "--");
}

void finder_print_bss(void)
{
    static bss_dump_t list[BSS_TABLE_LEN]; /* console task stack is small */
    int n = 0;
    uint32_t now = now_ms();

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < BSS_TABLE_LEN; i++) {
        bss_rec_t *b = &s_bss[i];
        if (!b->used) continue;
        memcpy(list[n].bssid, b->bssid, 6);
        memcpy(list[n].ssid, b->ssid, sizeof(list[n].ssid));
        list[n].ssid[FINDER_SSID_MAX - 1] = 0;
        list[n].ssid_len = b->ssid_len;
        list[n].ch = b->channel;
        list[n].rssi = b->rssi;
        list[n].last_ms = b->last_ms;
        list[n].has_load = b->has_load;
        list[n].load_sta = b->load_sta;
        list[n].load_util = b->load_util;
        list[n].load_aac = b->load_aac;
        n++;
    }
    xSemaphoreGive(s_tab_lock);

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (list[j].rssi > list[i].rssi) {
                bss_dump_t t = list[i];
                list[i] = list[j];
                list[j] = t;
            }
        }
    }

    ESP_LOGI(TAG, "%d known BSS(s)", n);
    int shown = n > 20 ? 20 : n;
    for (int i = 0; i < shown; i++) {
        char mac[18], age[16];
        mac_text(list[i].bssid, mac);
        snprintf(age, sizeof(age), "%lus", (unsigned long)((now - list[i].last_ms) / 1000));
        if (list[i].has_load) {
            ESP_LOGI(TAG, "%2d %s  %4d dBm  ch %u  %s  AP %u%%/%uSTA AAC%u  %s",
                     i + 1, mac, list[i].rssi, list[i].ch,
                     list[i].ssid_len ? list[i].ssid : "<hidden>",
                     (unsigned)((list[i].load_util * 100u + 127u) / 255u),
                     list[i].load_sta, list[i].load_aac, age);
        } else {
            ESP_LOGI(TAG, "%2d %s  %4d dBm  ch %u  %s  %s",
                     i + 1, mac, list[i].rssi, list[i].ch,
                     list[i].ssid_len ? list[i].ssid : "<hidden>", age);
        }
    }
}

static void dump_candidate(dump_t *d, const cand_rec_t *c, uint32_t now)
{
    memcpy(d->mac, c->mac, 6);
    memcpy(d->bssid, c->bssid, 6);
    d->last_ms = c->last_ms;
    d->frames = c->frames;
    d->rssi = c->rssi;
    d->ch = c->channel;
    d->ap_ssid_len = 0;
    d->probe_ssid_len = 0;
    d->ap_ssid[0] = 0;
    d->probe_ssid[0] = 0;
    d->ap_known = false;
    d->ap_hidden = false;

    bss_rec_t *b = bss_find(c->bssid);
    if (b) {
        d->ap_known = true;
        if (!b->ssid_len) d->ap_hidden = true;
    }
    if (b && b->ssid_len) {
        memcpy(d->ap_ssid, b->ssid, b->ssid_len);
        d->ap_ssid[b->ssid_len] = 0;
        d->ap_ssid_len = b->ssid_len;
    }
    if (c->probe_ssid_len) {
        memcpy(d->probe_ssid, c->probe_ssid, c->probe_ssid_len);
        d->probe_ssid[c->probe_ssid_len] = 0;
        d->probe_ssid_len = c->probe_ssid_len;
    }
    (void)now;
}

void finder_print_candidates(void)
{
    static dump_t list[CAND_TABLE_LEN]; /* console task stack is small */
    int n = 0;
    uint32_t now = now_ms();

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        cand_rec_t *c = &s_cand[i];
        if (!c->used || now - c->last_ms > ACTIVE_MS) continue;
        dump_candidate(&list[n], c, now);
        n++;
    }
    xSemaphoreGive(s_tab_lock);

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (list[j].last_ms > list[i].last_ms) {
                dump_t t = list[i];
                list[i] = list[j];
                list[j] = t;
            }
        }
    }

    ESP_LOGI(TAG, "%d recent candidate(s)", n);
    int shown = n > 16 ? 16 : n;
    for (int i = 0; i < shown; i++) {
        char mac[18], bss[18], age[16];
        mac_text(list[i].mac, mac);
        mac_text(list[i].bssid, bss);
        if (mac_zero(list[i].bssid)) snprintf(bss, sizeof(bss), "--");
        snprintf(age, sizeof(age), "%lus", (unsigned long)((now - list[i].last_ms) / 1000));

        char ssid[FINDER_SSID_MAX + 8];
        if (list[i].ap_ssid_len) {
            snprintf(ssid, sizeof(ssid), "SSID %s", list[i].ap_ssid);
        } else if (list[i].ap_hidden) {
            snprintf(ssid, sizeof(ssid), "SSID <hidden>");
        } else if (list[i].probe_ssid_len) {
            snprintf(ssid, sizeof(ssid), "probe %s", list[i].probe_ssid);
        } else {
            snprintf(ssid, sizeof(ssid), "SSID --");
        }

        ESP_LOGI(TAG, "%2d %s  %4d dBm  ch %u  %lu f  AP %s  %s  %s",
                 i + 1, mac, list[i].rssi, list[i].ch,
                 (unsigned long)list[i].frames, bss, ssid, age);
    }
}

void finder_print_stas_by_ap(void)
{
    static bss_dump_t aps[BSS_TABLE_LEN];   /* console task stack is small */
    static dump_t stas[CAND_TABLE_LEN];
    static bool sta_used[CAND_TABLE_LEN];
    int nap = 0, nsta = 0;
    uint32_t now = now_ms();

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < BSS_TABLE_LEN; i++) {
        bss_rec_t *b = &s_bss[i];
        if (!b->used) continue;
        memcpy(aps[nap].bssid, b->bssid, 6);
        memcpy(aps[nap].ssid, b->ssid, sizeof(aps[nap].ssid));
        aps[nap].ssid[FINDER_SSID_MAX - 1] = 0;
        aps[nap].ssid_len = b->ssid_len;
        aps[nap].ch = b->channel;
        aps[nap].rssi = b->rssi;
        aps[nap].last_ms = b->last_ms;
        aps[nap].has_load = b->has_load;
        aps[nap].load_sta = b->load_sta;
        aps[nap].load_util = b->load_util;
        aps[nap].load_aac = b->load_aac;
        nap++;
    }
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        cand_rec_t *c = &s_cand[i];
        if (!c->used || now - c->last_ms > ACTIVE_MS) continue;
        memcpy(stas[nsta].mac, c->mac, 6);
        memcpy(stas[nsta].bssid, c->bssid, 6);
        stas[nsta].last_ms = c->last_ms;
        stas[nsta].frames = c->frames;
        stas[nsta].rssi = c->rssi;
        stas[nsta].ch = c->channel;
        nsta++;
    }
    xSemaphoreGive(s_tab_lock);

    int seen[BSS_TABLE_LEN];
    int order[BSS_TABLE_LEN];
    for (int i = 0; i < nap; i++) {
        seen[i] = 0;
        order[i] = i;
        for (int j = 0; j < nsta; j++) {
            if (memcmp(stas[j].bssid, aps[i].bssid, 6) == 0) seen[i]++;
        }
    }
    /* APs with visible STAs first, then strongest RSSI. */
    for (int i = 0; i < nap; i++) {
        for (int j = i + 1; j < nap; j++) {
            int ai = order[i], aj = order[j];
            bool swap = false;
            if ((seen[ai] > 0) != (seen[aj] > 0)) swap = seen[aj] > 0;
            else if (seen[ai] != seen[aj]) swap = seen[aj] > seen[ai];
            else swap = aps[aj].rssi > aps[ai].rssi;
            if (swap) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    memset(sta_used, 0, sizeof(sta_used));
    ESP_LOGI(TAG, "%d known AP(s), %d active STA(s)", nap, nsta);
    int shown_aps = nap < 12 ? nap : 12;
    for (int oi = 0; oi < shown_aps; oi++) {
        int bi = order[oi];
        char line[192];
        int off = snprintf(line, sizeof(line), "AP %02X:%02X:%02X:%02X:%02X:%02X  ",
                           aps[bi].bssid[0], aps[bi].bssid[1], aps[bi].bssid[2],
                           aps[bi].bssid[3], aps[bi].bssid[4], aps[bi].bssid[5]);
        if (aps[bi].ssid_len)
            off += snprintf(line + off, sizeof(line) - off, "SSID %s", aps[bi].ssid);
        else
            off += snprintf(line + off, sizeof(line) - off, "SSID <hidden>");
        off += snprintf(line + off, sizeof(line) - off, "  ch %u", aps[bi].ch);
        if (aps[bi].has_load)
            off += snprintf(line + off, sizeof(line) - off, "  AP %u STA", aps[bi].load_sta);
        off += snprintf(line + off, sizeof(line) - off, "  seen %d", seen[bi]);

        int shown = 0;
        for (int j = 0; j < nsta; j++) {
            if (memcmp(stas[j].bssid, aps[bi].bssid, 6) != 0) continue;
            sta_used[j] = true;
            if (shown < 4 && off < (int)sizeof(line) - 24) {
                char mac[18];
                mac_text(stas[j].mac, mac);
                off += snprintf(line + off, sizeof(line) - off, "  %s(%d)",
                                mac, stas[j].rssi);
            }
            shown++;
        }
        if (shown > 4) snprintf(line + off, sizeof(line) - off, " +%d", shown - 4);
        ESP_LOGI(TAG, "%s", line);
    }

    /* Active STAs whose BSSID was not in the AP table (or was wildcard). */
    int unknown = 0;
    for (int j = 0; j < nsta; j++) {
        if (sta_used[j]) continue;
        int total = 0;
        for (int k = j; k < nsta; k++) {
            if (!sta_used[k] && memcmp(stas[k].bssid, stas[j].bssid, 6) == 0)
                total++;
        }
        char line[192];
        int off = snprintf(line, sizeof(line), "AP --  SSID --  ch %u  seen %d",
                           stas[j].ch, total);
        int shown = 0;
        for (int k = j; k < nsta && shown < 4; k++) {
            if (sta_used[k] || memcmp(stas[k].bssid, stas[j].bssid, 6) != 0) continue;
            sta_used[k] = true;
            char mac[18];
            mac_text(stas[k].mac, mac);
            off += snprintf(line + off, sizeof(line) - off, "  %s(%d)",
                            mac, stas[k].rssi);
            shown++;
        }
        if (total > shown) snprintf(line + off, sizeof(line) - off, " +%d", total - shown);
        ESP_LOGI(TAG, "%s", line);
        if (++unknown >= 6) break;
    }
    ESP_LOGI(TAG, "passive only: no probe/scan requests are sent");
}

bool finder_init(void)
{
    s_api_lock = xSemaphoreCreateMutex();
    s_tab_lock = xSemaphoreCreateMutex();
    s_evt_q = xQueueCreate(EVT_QUEUE_LEN, sizeof(snif_evt_t));
    if (!s_api_lock || !s_tab_lock || !s_evt_q) {
        ESP_LOGE(TAG, "no memory for finder objects");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) err = nvs_open("finder", NVS_READWRITE, &s_nvs);
    s_nvs_ok = (err == ESP_OK);
    if (!s_nvs_ok) ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(err));

    nvs_load_target();
    s_fps_sample_ms = now_ms();

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "netif init: %s", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init: %s", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        return false;
    }

    wifi_country_t country = {0};
    strcpy(country.cc, "01");
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = 20;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    err = esp_wifi_set_country(&country);
    if (err != ESP_OK) ESP_LOGW(TAG, "country: %s", esp_err_to_name(err));

    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    s_cur_ch = 1;
    esp_wifi_set_channel(s_cur_ch, WIFI_SECOND_CHAN_NONE);
    s_wifi_ok = true;
    ESP_LOGI(TAG, "Wi-Fi sniffer ready, hopping 1-%d", CH_MAX);
    return true;
}

void finder_poll(finder_view_t *v)
{
    if (!v) return;
    uint32_t now = now_ms();

    drain_events();

    v->wifi_ok = s_wifi_ok;
    v->have_target = s_target_set;
    if (s_target_set) memcpy(v->target, s_target, 6);
    else memset(v->target, 0, 6);

    uint32_t last;
    uint32_t frames;
    int8_t raw;
    bool have_bssid;
    uint8_t bssid[6];
    portENTER_CRITICAL(&s_stats_mux);
    last = s_t_last_ms;
    frames = s_t_frames;
    raw = s_t_rssi;
    have_bssid = s_t_have_bssid;
    memcpy(bssid, s_t_bssid, 6);
    portEXIT_CRITICAL(&s_stats_mux);

    v->frames_total = frames;
    v->have_rssi = s_target_set && last != 0 && s_rssi_n > 0;
    if (v->have_rssi) {
        v->rssi_raw = raw;
        v->rssi = rssi_median();
        v->age_ms = now - last;
        int p = ((int)v->rssi + 95) * 100 / 70;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        v->percent = (uint8_t)p;
        if (s_peak < -120 || raw > s_peak) s_peak = raw;
    } else {
        v->rssi = -100;
        v->rssi_raw = -100;
        v->age_ms = 0;
        v->percent = 0;
    }
    v->peak = s_peak;

    if (!s_target_set) v->link = FINDER_LINK_NONE;
    else if (!v->have_rssi) v->link = FINDER_LINK_LOST;
    else if (v->age_ms < 5000) v->link = FINDER_LINK_SEEN;
    else if (v->age_ms < 30000) v->link = FINDER_LINK_WEAK;
    else v->link = FINDER_LINK_LOST;

    if (frames >= s_fps_last_frames) {
        if (s_fps_sample_ms == 0) s_fps_sample_ms = now;
        if (now - s_fps_sample_ms >= 1000) {
            uint32_t d = frames - s_fps_last_frames;
            s_fps_hist[s_fps_i] = d;
            s_fps_i = (uint8_t)((s_fps_i + 1) % 4);
            if (s_fps_n < 4) s_fps_n++;
            s_fps_last_frames = frames;
            s_fps_sample_ms = now;
        }
    } else {
        s_fps_last_frames = frames;
        s_fps_sample_ms = now;
        s_fps_n = 0;
    }
    uint32_t sum = 0;
    for (int i = 0; i < s_fps_n; i++) sum += s_fps_hist[i];
    v->fps = s_fps_n ? sum / s_fps_n : 0;

    v->have_bssid = s_target_set && have_bssid;
    if (v->have_bssid) memcpy(v->bssid, bssid, 6);

    v->have_ssid = false;
    v->bss_known = false;
    v->ssid_hidden = false;
    v->ssid_nonascii = false;
    v->ssid[0] = 0;
    v->have_ap_load = false;
    if (v->have_bssid &&
        xSemaphoreTake(s_tab_lock, portMAX_DELAY) == pdTRUE) {
        bss_rec_t *b = bss_find(v->bssid);
        if (b) {
            v->bss_known = true;
            if (!b->ssid_len) v->ssid_hidden = true;
        }
        if (b && b->ssid_len) {
            memcpy(v->ssid, b->ssid, b->ssid_len);
            v->ssid[b->ssid_len] = 0;
            v->have_ssid = true;
            for (int i = 0; i < b->ssid_len; i++) {
                if ((uint8_t)v->ssid[i] & 0x80) v->ssid_nonascii = true;
            }
        }
        if (b && b->has_load) {
            v->have_ap_load = true;
            v->ap_load = b->load_util;
            v->ap_sta_count = b->load_sta;
        }
        xSemaphoreGive(s_tab_lock);
    }
    if (!v->have_ssid && s_ssid_hint_len) {
        memcpy(v->ssid, s_ssid_hint, s_ssid_hint_len);
        v->ssid[s_ssid_hint_len] = 0;
        v->have_ssid = true;
        for (int i = 0; i < s_ssid_hint_len; i++) {
            if ((uint8_t)v->ssid[i] & 0x80) v->ssid_nonascii = true;
        }
    }
    if (!v->have_ssid && s_target_set) {
        uint8_t len = 0;
        if (cand_probe_ssid(s_target, v->ssid, sizeof(v->ssid), &len)) {
            v->have_ssid = true;
            for (int i = 0; i < len; i++) {
                if ((uint8_t)v->ssid[i] & 0x80) v->ssid_nonascii = true;
            }
        }
    }

    v->vendor[0] = 0;
    if (s_target_set) {
        const char *name = oui_lookup(v->target);
        if (name) {
            strncpy(v->vendor, name, sizeof(v->vendor) - 1);
            v->vendor[sizeof(v->vendor) - 1] = 0;
        } else {
            strncpy(v->vendor, (s_target[0] & 0x02) ? "LOCAL ADMIN" : "UNKNOWN",
                    sizeof(v->vendor) - 1);
            v->vendor[sizeof(v->vendor) - 1] = 0;
        }
    }

    channel_logic(v, now);

    v->packets = s_packets;
    v->drops = s_drops;

    int active = 0;
    bool have_best = false;
    uint8_t best_mac[6] = {0};
    int8_t best_rssi = -128;
    uint32_t best_frames = 0;
    int32_t best_score = -1;
    bool learning = s_learn_until != 0 && now < s_learn_until;

    xSemaphoreTake(s_tab_lock, portMAX_DELAY);
    for (int i = 0; i < CAND_TABLE_LEN; i++) {
        cand_rec_t *c = &s_cand[i];
        if (!c->used || now - c->last_ms > ACTIVE_MS) continue;
        active++;

        int32_t score;
        int8_t use_rssi;
        uint32_t use_frames;
        if (learning) {
            if (!c->learn_frames) continue;
            use_rssi = c->learn_rssi;
            use_frames = c->learn_frames;
            score = (int32_t)(use_rssi + 100) * 100 +
                    (int32_t)(use_frames > 99 ? 99 : use_frames);
        } else {
            use_rssi = c->rssi;
            use_frames = c->frames;
            score = (int32_t)(use_rssi + 100) * 1000 +
                    (int32_t)(use_frames > 999 ? 999 : use_frames);
        }
        if (score > best_score) {
            best_score = score;
            have_best = true;
            memcpy(best_mac, c->mac, 6);
            best_rssi = use_rssi;
            best_frames = use_frames;
        }
    }
    xSemaphoreGive(s_tab_lock);

    v->cand_count = (uint32_t)active;
    v->have_best = have_best;
    if (have_best) {
        memcpy(v->best_mac, best_mac, 6);
        v->best_rssi = best_rssi;
        v->best_frames = best_frames;
    }

    if (s_learn_until != 0 && now >= s_learn_until) {
        uint8_t chosen[6] = {0};
        uint8_t chosen_ch = 0;
        bool found = false;
        int32_t best = -1;

        xSemaphoreTake(s_tab_lock, portMAX_DELAY);
        for (int i = 0; i < CAND_TABLE_LEN; i++) {
            cand_rec_t *c = &s_cand[i];
            if (!c->used || !c->learn_frames) continue;
            int32_t score = (int32_t)(c->learn_rssi + 100) * 1000 +
                            (int32_t)(c->learn_frames > 999 ? 999 : c->learn_frames);
            if (score > best) {
                best = score;
                memcpy(chosen, c->mac, 6);
                chosen_ch = c->channel;
                found = true;
            }
        }
        xSemaphoreGive(s_tab_lock);

        s_learn_until = 0;
        if (found) {
            finder_set_target(chosen, "learned");
            target_lock_channel(chosen_ch);
        } else {
            ESP_LOGW(TAG, "learning finished with no candidate");
            finder_toast("NO CANDIDATE");
        }
    }

    v->learning = s_learn_until != 0 && now < s_learn_until;
    v->learn_left_ms = v->learning ? (s_learn_until - now) : 0;

    portENTER_CRITICAL(&s_toast_mux);
    if (now < s_toast_until) {
        memcpy(v->toast, s_toast, sizeof(v->toast));
        v->toast[sizeof(v->toast) - 1] = 0;
        v->toast_until = s_toast_until;
    } else {
        v->toast[0] = 0;
        v->toast_until = 0;
    }
    portEXIT_CRITICAL(&s_toast_mux);
}
