#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Optional 1-bit RPD side channel.  It follows the current Wi-Fi channel and
 * reports whether the NRF sees energy near that channel centre. */
void nrf_aux_start(void);
void nrf_aux_set_channel(uint8_t wifi_channel);
bool nrf_aux_ok(void);
bool nrf_aux_ok2(void);
int nrf_aux_csn(void);
int nrf_aux_csn2(void);
uint8_t nrf_aux_duty(void);

#define NRF_SCAN_CHANNELS 13

typedef struct {
    uint8_t peak;    /* max RPD duty over the scan, 0..100 */
    uint8_t avg;     /* smoothed RPD duty, 0..100 */
    uint8_t last;    /* latest RPD duty, 0..100 */
    uint16_t hits;   /* sweeps with >=25% duty */
    uint16_t hits_10s; /* hits in the last 10 seconds */
    uint16_t sweeps; /* total sweeps for this channel */
    uint8_t streak;  /* consecutive hit sweeps */
    uint8_t max_streak;
    uint32_t peak_age_ms; /* time since the peak was last seen */
    uint32_t last_age_ms; /* time since any energy was seen */
} nrf_scan_ch_t;

/* Channel-energy scan.  While active the NRF leaves the Wi-Fi channel and
 * steps 1..13, holding the peak value per channel. */
void nrf_aux_scan(bool on);
bool nrf_aux_scan_active(void);
void nrf_aux_scan_get(nrf_scan_ch_t *out, int max);
uint32_t nrf_aux_scan_cycles(void);
uint32_t nrf_aux_scan_elapsed_ms(void);

/* Fixed 2450 MHz observation mode (NRF RF_CH=50).  Samples back-to-back
 * 8 ms RPD windows instead of hopping the 13 Wi-Fi channels. */
typedef struct {
    uint8_t peak;      /* max RPD duty since reset, 0..100 */
    uint8_t avg;       /* smoothed RPD duty */
    uint8_t last;
    uint32_t hits;     /* windows with >=25% duty */
    uint16_t hits_10s; /* hits in the last 10 seconds */
    uint32_t sweeps;   /* total 8 ms windows */
    uint32_t peak_age_ms;
    uint32_t last_age_ms;
} nrf_mw_t;

void nrf_aux_mw(bool on);
bool nrf_aux_mw_active(void);
void nrf_aux_mw_reset(void);
void nrf_aux_mw_get(nrf_mw_t *out);
uint32_t nrf_aux_mw_elapsed_ms(void);

/* RF SCAN PRO: step all 126 NRF channels (2400..2525 MHz, 1 MHz grid) and
 * keep a peak/hit map for each one. */
#define NRF_PRO_CHANNELS 126

typedef struct {
    uint8_t peak;    /* max RPD duty, 0..100 */
    uint8_t heat;    /* decayed recent activity, 0..100 */
    uint8_t last;    /* latest RPD duty */
    uint8_t avg;     /* smoothed RPD duty */
    uint16_t hits;   /* visits with >=25% duty */
    uint16_t sweeps; /* visits */
} nrf_pro_ch_t;

void nrf_aux_pro(bool on);
bool nrf_aux_pro_active(void);
void nrf_aux_pro_reset(void);
void nrf_aux_pro_get(nrf_pro_ch_t *out, int max);
uint32_t nrf_aux_pro_cycles(void);
uint32_t nrf_aux_pro_elapsed_ms(void);
