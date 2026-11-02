#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"

/* Dedicated SPI3 pins.  The ST7735 keeps SPI2, so the two buses never mix. */
#define NRF_SCK_PIN   13
#define NRF_MOSI_PIN  15
#define NRF_MISO_PIN  16

/* Set to a free GPIO if the NRF 3.3V rail goes through a P-MOSFET/load
 * switch.  -1 means the modules are powered directly. */
#define NRF_PWR_EN_PIN (-1)

#define NRF_MODULE_COUNT 3
#define NRF_CSN_PINS {17, 18, 21}
#define NRF_CE_PINS  {38, 39, 40}

typedef struct {
    spi_device_handle_t spi;
    int ce_pin;
    int csn_pin;
    bool present;
} nrf_t;

bool nrf_bus_init(void);
bool nrf_init(nrf_t *n, int csn_pin, int ce_pin);
void nrf_standby(nrf_t *n);

void nrf_set_channel(nrf_t *n, uint8_t channel);
void nrf_set_address(nrf_t *n, const uint8_t addr[5]);
void nrf_tx_mode(nrf_t *n);
void nrf_rx_mode(nrf_t *n);

/* Returns true on TX_DS (ACK received), false on MAX_RT/timeout.  observe is
 * the OBSERVE_TX register (ARC_CNT in bits 3:0). */
bool nrf_send(nrf_t *n, const uint8_t *payload, size_t len, uint8_t *observe);
bool nrf_rx_poll(nrf_t *n, uint8_t *payload, size_t len);

uint8_t nrf_read_reg(nrf_t *n, uint8_t reg);
void nrf_write_reg(nrf_t *n, uint8_t reg, uint8_t value);
