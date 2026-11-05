#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FINDER_SSID_MAX 33
#define FINDER_VENDOR_MAX 48
#define FINDER_TOAST_MAX 48
#define FINDER_AP_MAX 16
#define FINDER_STA_MAX 16

typedef struct {
    uint8_t bssid[6];
    char ssid[FINDER_SSID_MAX];
    uint8_t ssid_len;
    uint8_t channel;
    int8_t rssi;
    bool hidden;
    bool binary;
    bool has_load;
    uint16_t load_sta;
    uint8_t load_util;
    uint8_t seen_sta;
    uint32_t last_ms;
    char vendor[FINDER_VENDOR_MAX];
} finder_ap_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t frames;
    uint32_t last_ms;
    uint8_t channel;
    bool local;
    char vendor[FINDER_VENDOR_MAX];
    char probe_ssid[FINDER_SSID_MAX];
    uint8_t probe_ssid_len;
} finder_sta_t;

typedef enum {
    FINDER_LINK_NONE = 0,
    FINDER_LINK_SEEN,
    FINDER_LINK_WEAK,
    FINDER_LINK_LOST,
} finder_link_t;

typedef enum {
    FINDER_CH_AUTO = 0,
    FINDER_CH_LOCKED,
    FINDER_CH_MANUAL,
} finder_chan_mode_t;

typedef struct {
    bool wifi_ok;

    bool have_target;
    uint8_t target[6];

    bool learning;
    uint32_t learn_left_ms;

    bool have_rssi;
    int8_t rssi;       /* median of the last five target frames */
    int8_t rssi_raw;   /* most recent target frame */
    int8_t peak;
    uint8_t percent;   /* -95..-25 dBm mapped to 0..100 */
    uint32_t fps;      /* smoothed matching frames per second */
    uint32_t age_ms;

    uint32_t frames_total;
    uint32_t packets;
    uint32_t drops;
    uint32_t cand_count;

    uint8_t channel;        /* current sniffer channel */
    uint8_t target_channel; /* last channel the target was heard on */
    finder_chan_mode_t chan_mode;

    bool have_bssid;
    uint8_t bssid[6];
    bool have_ap_load;
    uint8_t ap_load;       /* raw 802.11 channel utilization 0..255 */
    uint16_t ap_sta_count;
    bool have_ssid;
    bool bss_known;
    bool ssid_hidden;
    bool ssid_nonascii;
    char ssid[FINDER_SSID_MAX];
    char vendor[FINDER_VENDOR_MAX];

    finder_link_t link;

    bool have_best;
    uint8_t best_mac[6];
    int8_t best_rssi;
    uint32_t best_frames;

    bool nrf_ok;
    bool nrf_ok2;
    int8_t nrf_csn;
    int8_t nrf_csn2;
    uint8_t nrf_duty;

    char toast[FINDER_TOAST_MAX];
    uint32_t toast_until;
} finder_view_t;

bool finder_init(void);
void finder_poll(finder_view_t *v);
bool finder_wifi_pause(void);
bool finder_wifi_resume(void);

bool finder_set_target(const uint8_t mac[6], const char *why);
void finder_set_target_temp(const uint8_t mac[6], const char *why, uint8_t ch);
bool finder_set_target_str(const char *text);
void finder_clear_target(void);
void finder_next_candidate(void);
void finder_prev_candidate(void);
void finder_start_learn(void);
void finder_toggle_lock(void);
void finder_next_channel(void);
void finder_prev_channel(void);
void finder_clear_peak(void);

int finder_snapshot_aps(finder_ap_t *out, int max);
int finder_snapshot_stas(const uint8_t bssid[6], finder_sta_t *out, int max);
void finder_print_candidates(void);
void finder_print_stas_by_ap(void);
void finder_print_bss(void);
void finder_print_wnm(void);
void finder_toast(const char *fmt, ...);
