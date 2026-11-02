#include "nrf_aux.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nrf24.h"

#define REG_RF_CH 0x05
#define REG_RPD   0x09

#define SAMPLES 8
#define SAMPLE_US 500

static const char *TAG = "nrf_aux";
static nrf_t s_radio;
static volatile bool s_ok;
static volatile uint8_t s_duty;
static volatile uint8_t s_wifi_ch = 1;
static bool s_bus_ready;
static bool s_logged_found;
static bool s_logged_lost;
static uint32_t s_last_fail_ms;
static const int s_csn[NRF_MODULE_COUNT] = NRF_CSN_PINS;
static const int s_ce[NRF_MODULE_COUNT] = NRF_CE_PINS;

static uint8_t wifi_to_rf(uint8_t ch)
{
    if (ch < 1) ch = 1;
    if (ch > 13) ch = 13;
    int mhz = 2407 + (int)ch * 5;
    int rf = mhz - 2400;
    if (rf < 0) rf = 0;
    if (rf > 125) rf = 125;
    return (uint8_t)rf;
}

static bool open_radio(void)
{
    if (!s_bus_ready) {
        if (!nrf_bus_init()) return false;
        s_bus_ready = true;
    }
    /* NRF24L01+ modules are slow to settle after power-up.  Probe every
     * CSN/CE pair several times with a short pause before giving up. */
    for (int pass = 0; pass < 4; pass++) {
        for (int i = 0; i < NRF_MODULE_COUNT; i++) {
            if (nrf_init(&s_radio, s_csn[i], s_ce[i])) {
                if (!s_logged_found) {
                    ESP_LOGI(TAG, "NRF24L01 on CSN %d", s_csn[i]);
                    s_logged_found = true;
                }
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return false;
}

static void nrf_aux_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500)); /* give the slow modules time to settle */

    for (;;) {
        if (!s_ok) {
            if (!open_radio()) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                if ((uint32_t)(now - s_last_fail_ms) >= 30000) {
                    s_last_fail_ms = now;
                    ESP_LOGW(TAG, "NRF not found on CSN 17/18/21, retrying");
                }
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            s_ok = true;
        }

        uint8_t rf = wifi_to_rf(s_wifi_ch);
        nrf_set_channel(&s_radio, rf);
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t got = 0;
        for (int attempt = 0; attempt < 4; attempt++) {
            got = nrf_read_reg(&s_radio, REG_RF_CH);
            if (got == rf) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (got != rf) {
            if (!s_logged_lost) {
                ESP_LOGW(TAG, "NRF link unstable: rf %u read %u", rf, got);
                s_logged_lost = true;
            }
            s_ok = false;
            s_duty = 0;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        s_logged_lost = false;

        nrf_rx_mode(&s_radio);
        esp_rom_delay_us(200);

        unsigned high = 0;
        for (int i = 0; i < SAMPLES; i++) {
            if (nrf_read_reg(&s_radio, REG_RPD) & 1u) high++;
            esp_rom_delay_us(SAMPLE_US);
        }
        nrf_standby(&s_radio);
        s_duty = (uint8_t)(high * 100u / SAMPLES);

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void nrf_aux_start(void)
{
    /* The driver logs one warning per probed CSN when no module is fitted.
     * The UI already shows RF --, so keep the serial log quiet. */
    esp_log_level_set("nrf24", ESP_LOG_ERROR);
    xTaskCreatePinnedToCore(nrf_aux_task, "nrf_aux", 4096, NULL, 3, NULL, 1);
}

void nrf_aux_set_channel(uint8_t wifi_channel)
{
    s_wifi_ch = wifi_channel;
}

int nrf_aux_csn(void)
{
    return s_ok ? s_radio.csn_pin : -1;
}

bool nrf_aux_ok(void)
{
    return s_ok;
}

uint8_t nrf_aux_duty(void)
{
    return s_duty;
}
