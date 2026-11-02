#include "nrf24.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_OBSERVE_TX  0x08
#define REG_RX_ADDR_P0  0x0A
#define REG_TX_ADDR     0x10
#define REG_RX_PW_P0    0x11

#define CMD_R_RX_PAYLOAD 0x61
#define CMD_W_TX_PAYLOAD 0xA0
#define CMD_FLUSH_TX     0xE1
#define CMD_FLUSH_RX     0xE2

#define STATUS_RX_DR  0x40
#define STATUS_TX_DS  0x20
#define STATUS_MAX_RT 0x10

static const char *TAG = "nrf24";

static void xfer(nrf_t *n, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(n->spi, &t));
}

static void cmd1(nrf_t *n, uint8_t cmd)
{
    xfer(n, &cmd, NULL, 1);
}

static void write_reg_buf(nrf_t *n, uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t tx[40];
    tx[0] = (uint8_t)(0x20 | (reg & 0x1F));
    memcpy(&tx[1], data, len);
    xfer(n, tx, NULL, len + 1);
}

static void write_cmd_buf(nrf_t *n, uint8_t cmd, const uint8_t *data, size_t len)
{
    uint8_t tx[40];
    tx[0] = cmd;
    memcpy(&tx[1], data, len);
    xfer(n, tx, NULL, len + 1);
}

static void read_buf(nrf_t *n, uint8_t cmd, uint8_t *data, size_t len)
{
    uint8_t tx[40], rx[40];
    tx[0] = cmd;
    memset(&tx[1], 0xFF, len);
    xfer(n, tx, rx, len + 1);
    memcpy(data, &rx[1], len);
}

static void flush_tx(nrf_t *n) { cmd1(n, CMD_FLUSH_TX); }
static void flush_rx(nrf_t *n) { cmd1(n, CMD_FLUSH_RX); }

uint8_t nrf_read_reg(nrf_t *n, uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0};
    xfer(n, tx, rx, sizeof(tx));
    return rx[1];
}

void nrf_write_reg(nrf_t *n, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(0x20 | (reg & 0x1F)), value};
    xfer(n, tx, NULL, sizeof(tx));
}

bool nrf_bus_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = NRF_MOSI_PIN,
        .miso_io_num = NRF_MISO_PIN,
        .sclk_io_num = NRF_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) return true; /* already up */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool nrf_init(nrf_t *n, int csn_pin, int ce_pin)
{
    n->present = false;

    /* Re-probing another CSN must not keep talking to the first device. */
    if (n->spi && n->csn_pin != csn_pin) {
        spi_bus_remove_device(n->spi);
        n->spi = NULL;
    }
    n->csn_pin = csn_pin;
    n->ce_pin = ce_pin;
    if (n->spi == NULL) {
        const spi_device_interface_config_t dev = {
            .clock_speed_hz = 2 * 1000 * 1000,
            .mode = 0,
            .spics_io_num = csn_pin,
            .queue_size = 1,
        };
        if (spi_bus_add_device(SPI3_HOST, &dev, &n->spi) != ESP_OK) {
            ESP_LOGE(TAG, "no SPI device for CSN %d", csn_pin);
            return false;
        }
    }

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << ce_pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_set_level(ce_pin, 0));

    /* A missing module reads 0x00 or 0xFF.  Write two registers and read
     * them back.  The module can need a few ms after power-up, so retry. */
    bool ok = false;
    for (int attempt = 0; attempt < 4; attempt++) {
        nrf_write_reg(n, REG_CONFIG, 0x0E);   /* EN_CRC, CRCO=2, PWR_UP */
        nrf_write_reg(n, REG_RF_CH, 0x4B);    /* temporary test value */
        vTaskDelay(pdMS_TO_TICKS(10));
        if (nrf_read_reg(n, REG_CONFIG) == 0x0E &&
            nrf_read_reg(n, REG_RF_CH) == 0x4B) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!ok) {
        ESP_LOGW(TAG, "no NRF24L01 on CSN %d", csn_pin);
        return false;
    }

    nrf_write_reg(n, REG_EN_AA, 0x01);       /* auto-ACK pipe 0 */
    nrf_write_reg(n, REG_EN_RXADDR, 0x01);   /* pipe 0 */
    nrf_write_reg(n, REG_SETUP_AW, 0x03);    /* 5-byte address */
    nrf_write_reg(n, REG_SETUP_RETR, 0x13);  /* 500 us, 3 retries */
    nrf_write_reg(n, REG_RF_CH, 80);         /* 2480 MHz */
    nrf_write_reg(n, REG_RF_SETUP, 0x06);    /* 1 Mbps, PA max (clone-safe) */
    nrf_write_reg(n, REG_RX_PW_P0, 8);
    nrf_write_reg(n, REG_STATUS, 0x70);
    flush_tx(n);
    flush_rx(n);

    n->present = true;
    ESP_LOGI(TAG, "NRF24L01 ready on CSN %d", csn_pin);
    return true;
}

void nrf_standby(nrf_t *n)
{
    if (!n->present) return;
    ESP_ERROR_CHECK(gpio_set_level(n->ce_pin, 0));
}

void nrf_set_channel(nrf_t *n, uint8_t channel)
{
    if (!n->present) return;
    nrf_write_reg(n, REG_RF_CH, (uint8_t)(channel & 0x7F));
}

void nrf_set_address(nrf_t *n, const uint8_t addr[5])
{
    if (!n->present) return;
    write_reg_buf(n, REG_TX_ADDR, addr, 5);
    write_reg_buf(n, REG_RX_ADDR_P0, addr, 5);
}

void nrf_tx_mode(nrf_t *n)
{
    if (!n->present) return;
    gpio_set_level(n->ce_pin, 0);
    uint8_t cfg = nrf_read_reg(n, REG_CONFIG);
    nrf_write_reg(n, REG_CONFIG, (uint8_t)(cfg & ~0x01));
    flush_tx(n);
    nrf_write_reg(n, REG_STATUS, 0x70);
    esp_rom_delay_us(150);
}

void nrf_rx_mode(nrf_t *n)
{
    if (!n->present) return;
    gpio_set_level(n->ce_pin, 0);
    uint8_t cfg = nrf_read_reg(n, REG_CONFIG);
    nrf_write_reg(n, REG_CONFIG, (uint8_t)(cfg | 0x01));
    flush_rx(n);
    nrf_write_reg(n, REG_STATUS, 0x70);
    gpio_set_level(n->ce_pin, 1);
    esp_rom_delay_us(150);
}

bool nrf_send(nrf_t *n, const uint8_t *payload, size_t len, uint8_t *observe)
{
    if (!n->present || len == 0 || len > 32) return false;

    gpio_set_level(n->ce_pin, 0);
    write_cmd_buf(n, CMD_W_TX_PAYLOAD, payload, len);
    gpio_set_level(n->ce_pin, 1);
    esp_rom_delay_us(20);
    gpio_set_level(n->ce_pin, 0);

    bool ok = false;
    for (int i = 0; i < 80; i++) {
        uint8_t status = nrf_read_reg(n, REG_STATUS);
        if (status & STATUS_TX_DS) { ok = true; break; }
        if (status & STATUS_MAX_RT) break;
        esp_rom_delay_us(50);
    }
    if (observe) *observe = nrf_read_reg(n, REG_OBSERVE_TX);
    nrf_write_reg(n, REG_STATUS, 0x70);
    return ok;
}

bool nrf_rx_poll(nrf_t *n, uint8_t *payload, size_t len)
{
    if (!n->present) return false;
    uint8_t status = nrf_read_reg(n, REG_STATUS);
    if (!(status & STATUS_RX_DR)) return false;
    read_buf(n, CMD_R_RX_PAYLOAD, payload, len);
    nrf_write_reg(n, REG_STATUS, STATUS_RX_DR);
    return true;
}
