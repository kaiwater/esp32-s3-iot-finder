#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Optional 1-bit RPD side channel.  It follows the current Wi-Fi channel and
 * reports whether the NRF sees energy near that channel centre. */
void nrf_aux_start(void);
void nrf_aux_set_channel(uint8_t wifi_channel);
bool nrf_aux_ok(void);
int nrf_aux_csn(void);
uint8_t nrf_aux_duty(void);
