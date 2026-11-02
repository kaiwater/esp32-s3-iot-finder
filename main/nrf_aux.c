#include "nrf_aux.h"

#include <string.h>

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
static nrf_t s_radio2;
static volatile bool s_ok;
static volatile bool s_ok2;
static volatile uint8_t s_duty;
static volatile uint8_t s_wifi_ch = 1;
static bool s_bus_ready;
static bool s_logged_found;
static bool s_logged_lost;
static uint32_t s_last_fail_ms;
static uint32_t s_last_second_try_ms;
static volatile bool s_scan;
static volatile uint32_t s_scan_cycles;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static nrf_scan_ch_t s_scan_data[NRF_SCAN_CHANNELS];
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

    /* Probe each CSN/CE pair as an independent SPI device.  Keeping a
     * separate nrf_t per pair is what allows both modules to come online;
     * the old single-radio path stopped at the first one. */
    static nrf_t cand[NRF_MODULE_COUNT];
    nrf_t found[2];
    int nfound = 0;
    s_ok2 = false;

    for (int i = 0; i < NRF_MODULE_COUNT && nfound < 2; i++) {
        for (int attempt = 0; attempt < 4; attempt++) {
            if (nrf_init(&cand[i], s_csn[i], s_ce[i])) {
                found[nfound++] = cand[i];
                if (nfound == 1 && i + 1 < NRF_MODULE_COUNT) {
                    /* Stagger the second module's power-up so both do not
                     * draw their startup current at the same instant. */
                    vTaskDelay(pdMS_TO_TICKS(600));
                }
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    if (!nfound) return false;

    s_radio = found[0];
    s_ok = true;
    if (nfound > 1) {
        s_radio2 = found[1];
        s_ok2 = true;
    }

    if (!s_logged_found) {
        if (s_ok2) {
            ESP_LOGI(TAG, "NRF24L01 CSN %d + %d",
                     s_radio.csn_pin, s_radio2.csn_pin);
        } else {
            ESP_LOGI(TAG, "NRF24L01 CSN %d (single)", s_radio.csn_pin);
        }
        s_logged_found = true;
    }
    return true;
}

static void scan_store(int ch, uint8_t pct)
{
    portENTER_CRITICAL(&s_scan_mux);
    nrf_scan_ch_t *sc = &s_scan_data[ch - 1];
    sc->last = pct;
    if (pct > sc->peak) sc->peak = pct;
    sc->avg = (uint8_t)((sc->avg * 3u + pct) / 4u);
    portEXIT_CRITICAL(&s_scan_mux);
}

static void try_second_radio(void)
{
    if (!s_ok || s_ok2) return;

    static nrf_t cand;
    static int next;
    for (int n = 0; n < NRF_MODULE_COUNT; n++) {
        int i = next;
        next = (next + 1) % NRF_MODULE_COUNT;
        if (s_csn[i] == s_radio.csn_pin) continue;
        if (nrf_init(&cand, s_csn[i], s_ce[i])) {
            s_radio2 = cand;
            s_ok2 = true;
            ESP_LOGI(TAG, "NRF hot-plug CSN %d", s_csn[i]);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void nrf_aux_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000)); /* give the slow modules time to settle */

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

        if (!s_ok2) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if ((uint32_t)(now - s_last_second_try_ms) >= 5000) {
                s_last_second_try_ms = now;
                try_second_radio();
            }
        }

        if (s_scan) {
            if (s_ok && s_ok2) {
                /* Two radios scan two channels per pass: 1..6 with 8..13;
                 * channel 7 is handled by radio 1 alone. */
                for (int ch = 1; ch <= 7 && s_scan; ch++) {
                    int ch2 = ch + 7;
                    bool use2 = ch2 <= NRF_SCAN_CHANNELS;
                    uint8_t rf1 = wifi_to_rf((uint8_t)ch);
                    uint8_t rf2 = use2 ? wifi_to_rf((uint8_t)ch2) : 0;
                    nrf_set_channel(&s_radio, rf1);
                    if (use2) nrf_set_channel(&s_radio2, rf2);
                    vTaskDelay(pdMS_TO_TICKS(5));
                    if (use2 &&
                        nrf_read_reg(&s_radio2, REG_RF_CH) != rf2) {
                        s_ok2 = false;
                        use2 = false;
                    }
                    nrf_rx_mode(&s_radio);
                    if (use2) nrf_rx_mode(&s_radio2);
                    vTaskDelay(pdMS_TO_TICKS(2));
                    unsigned h1 = 0, h2 = 0;
                    for (int i = 0; i < 16; i++) {
                        if (nrf_read_reg(&s_radio, REG_RPD) & 1u) h1++;
                        if (use2 && (nrf_read_reg(&s_radio2, REG_RPD) & 1u)) h2++;
                        esp_rom_delay_us(500);
                    }
                    nrf_standby(&s_radio);
                    if (use2) nrf_standby(&s_radio2);
                    scan_store(ch, (uint8_t)(h1 * 100u / 16u));
                    if (use2) scan_store(ch2, (uint8_t)(h2 * 100u / 16u));
                }
            } else {
                for (int ch = 1; ch <= NRF_SCAN_CHANNELS && s_scan; ch++) {
                    nrf_set_channel(&s_radio, wifi_to_rf((uint8_t)ch));
                    vTaskDelay(pdMS_TO_TICKS(5));
                    /* RPD is only valid with CE high in RX mode. */
                    nrf_rx_mode(&s_radio);
                    vTaskDelay(pdMS_TO_TICKS(2));
                    unsigned high = 0;
                    for (int i = 0; i < 16; i++) {
                        if (nrf_read_reg(&s_radio, REG_RPD) & 1u) high++;
                        esp_rom_delay_us(500);
                    }
                    nrf_standby(&s_radio);
                    scan_store(ch, (uint8_t)(high * 100u / 16u));
                }
            }
            s_scan_cycles++;
            s_duty = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint8_t rf = wifi_to_rf(s_wifi_ch);
        nrf_set_channel(&s_radio, rf);
        vTaskDelay(pdMS_TO_TICKS(10));

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

int nrf_aux_csn2(void)
{
    return s_ok2 ? s_radio2.csn_pin : -1;
}

bool nrf_aux_ok2(void)
{
    return s_ok2;
}

bool nrf_aux_ok(void)
{
    return s_ok;
}

uint8_t nrf_aux_duty(void)
{
    return s_duty;
}

void nrf_aux_scan(bool on)
{
    if (on) {
        portENTER_CRITICAL(&s_scan_mux);
        memset(s_scan_data, 0, sizeof(s_scan_data));
        s_scan_cycles = 0;
        portEXIT_CRITICAL(&s_scan_mux);
    }
    s_scan = on;
}

bool nrf_aux_scan_active(void)
{
    return s_scan;
}

void nrf_aux_scan_get(nrf_scan_ch_t *out, int max)
{
    if (!out || max <= 0) return;
    if (max > NRF_SCAN_CHANNELS) max = NRF_SCAN_CHANNELS;
    portENTER_CRITICAL(&s_scan_mux);
    memcpy(out, s_scan_data, (size_t)max * sizeof(*out));
    portEXIT_CRITICAL(&s_scan_mux);
}

uint32_t nrf_aux_scan_cycles(void)
{
    return s_scan_cycles;
}
