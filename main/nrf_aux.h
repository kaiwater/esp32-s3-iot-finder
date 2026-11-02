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
