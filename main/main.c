#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "finder.h"
#include "nrf_aux.h"
#include "st7735.h"
#include "wnm_clock.h"

static const char *TAG = "iot-finder";
static volatile int s_button_event;
static finder_view_t s_view;
static wnm_clock_view_t s_clock;
static int s_page;

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        int b = st7735_button();
        if (b) s_button_event = b;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void handle_event(int b)
{
    switch (b) {
    case 1:
        finder_toggle_lock();
        break;
    case 11:
        finder_next_channel();
        break;
    case 2:
        finder_clear_peak();
        break;
    case 12:
        finder_start_learn();
        break;
    case 3:
        finder_next_candidate();
        break;
    case 13:
        finder_clear_target();
        break;
    case 4:
        s_page = !s_page;
        break;
    case 14:
        esp_restart();
        break;
    default:
        break;
    }
}

static void print_help(void)
{
    ESP_LOGI("cli", "commands:");
    ESP_LOGI("cli", "  mac AA:BB:CC:DD:EE:FF  set target");
    ESP_LOGI("cli", "  clear                 remove target");
    ESP_LOGI("cli", "  learn                 start 5s learning");
    ESP_LOGI("cli", "  next                  select next candidate");
    ESP_LOGI("cli", "  lock                  toggle channel lock");
    ESP_LOGI("cli", "  chan                  manual next channel");
    ESP_LOGI("cli", "  peak                  clear peak");
    ESP_LOGI("cli", "  list                  print recent candidates");
    ESP_LOGI("cli", "  bss                   print recent BSSID/SSID");
    ESP_LOGI("cli", "  clock                 print WNM time / time zone");
    ESP_LOGI("cli", "  wnm                   print time-element counters");
    ESP_LOGI("cli", "  tz CST-8              manual POSIX time zone fallback");
    ESP_LOGI("cli", "  status                print current state");
    ESP_LOGI("cli", "  reboot                restart");
}

static void print_clock(void)
{
    if (!s_clock.synced) {
        ESP_LOGI("cli", "clock=-- tz=%s", s_clock.tz_name[0] ? s_clock.tz_name : "--");
        return;
    }
    char src[18] = "--";
    if (s_clock.have_source) {
        snprintf(src, sizeof(src), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_clock.source[0], s_clock.source[1], s_clock.source[2],
                 s_clock.source[3], s_clock.source[4], s_clock.source[5]);
    }
    ESP_LOGI("cli", "clock=%04u-%02u-%02u %02u:%02u:%02u tz=%s off=%+ld age=%lu src=%s",
             s_clock.year, s_clock.month, s_clock.day, s_clock.hour, s_clock.min,
             s_clock.sec, s_clock.tz_name[0] ? s_clock.tz_name : "UTC",
             (long)s_clock.offset_sec, (unsigned long)s_clock.age_ms, src);
}

static void print_status(void)
{
    char mac[18] = "--";
    if (s_view.have_target) {
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_view.target[0], s_view.target[1], s_view.target[2],
                 s_view.target[3], s_view.target[4], s_view.target[5]);
    }
    char bssid[18] = "--";
    if (s_view.have_bssid) {
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_view.bssid[0], s_view.bssid[1], s_view.bssid[2],
                 s_view.bssid[3], s_view.bssid[4], s_view.bssid[5]);
    }
    ESP_LOGI("cli", "wifi=%d target=%s rssi=%d fps=%lu ch=%u mode=%d state=%d cand=%lu pkt=%lu drop=%lu nrf=%d/%u%% heap=%lu",
             s_view.wifi_ok, mac, s_view.have_rssi ? s_view.rssi : -100,
             (unsigned long)s_view.fps, s_view.channel, s_view.chan_mode,
             s_view.link, (unsigned long)s_view.cand_count,
             (unsigned long)s_view.packets, (unsigned long)s_view.drops,
             s_view.nrf_ok, s_view.nrf_duty,
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI("cli", "vendor=%s bssid=%s ssid=%s peak=%d",
             s_view.vendor[0] ? s_view.vendor : "--", bssid,
             s_view.have_ssid ? s_view.ssid : "--", s_view.peak);
    print_clock();
}

static void handle_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) line++;
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) *--end = 0;
    if (!*line) return;

    if (!strncmp(line, "mac ", 4) || !strncmp(line, "set ", 4)) {
        const char *arg = line + 4;
        if (finder_set_target_str(arg)) {
            ESP_LOGI("cli", "target accepted");
        } else {
            ESP_LOGW("cli", "bad MAC (use AA:BB:CC:DD:EE:FF)");
        }
    } else if (!strcmp(line, "clear")) {
        finder_clear_target();
    } else if (!strcmp(line, "learn")) {
        finder_start_learn();
    } else if (!strcmp(line, "next")) {
        finder_next_candidate();
    } else if (!strcmp(line, "lock")) {
        finder_toggle_lock();
    } else if (!strcmp(line, "chan")) {
        finder_next_channel();
    } else if (!strcmp(line, "peak")) {
        finder_clear_peak();
    } else if (!strcmp(line, "list") || !strcmp(line, "cand")) {
        finder_print_candidates();
    } else if (!strcmp(line, "bss") || !strcmp(line, "aps")) {
        finder_print_bss();
    } else if (!strcmp(line, "status")) {
        print_status();
    } else if (!strcmp(line, "clock") || !strcmp(line, "time")) {
        print_clock();
    } else if (!strcmp(line, "wnm")) {
        finder_print_wnm();
    } else if (!strncmp(line, "tz ", 3)) {
        const char *arg = line + 3;
        wnm_clock_set_tz((const uint8_t *)arg, (uint8_t)strlen(arg), NULL);
    } else if (!strcmp(line, "reboot")) {
        esp_restart();
    } else if (!strcmp(line, "help") || !strcmp(line, "?")) {
        print_help();
    } else {
        ESP_LOGW("cli", "unknown command: %s", line);
        print_help();
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[80];
    size_t n = 0;

    for (;;) {
        uint8_t c;
        int r = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(200));
        if (r != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                handle_line(line);
                n = 0;
            }
        } else if (c == 0x08 || c == 0x7F) {
            if (n) n--;
        } else if (n + 1 < sizeof(line)) {
            line[n++] = (char)c;
        }
    }
}

static void console_start(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t cfg = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_param_config(UART_NUM_0, &cfg) != ESP_OK ||
            uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0) != ESP_OK) {
            ESP_LOGW("cli", "UART console unavailable");
            return;
        }
    }
    xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 2, NULL, 1);
    ESP_LOGI("cli", "type 'help' for commands");
}

void app_main(void)
{
    esp_log_level_set("st7735", ESP_LOG_WARN);
    st7735_init();

    memset(&s_view, 0, sizeof(s_view));
    s_view.link = FINDER_LINK_NONE;
    s_view.wifi_ok = finder_init();
    wnm_clock_init();
    wnm_clock_poll(&s_clock);
    st7735_draw_finder(&s_view, &s_clock, 0);

    nrf_aux_start();
    xTaskCreatePinnedToCore(button_task, "buttons", 2048, NULL, 2, NULL, 1);
    console_start();

    uint32_t last_draw = 0;
    uint32_t last_nrf = 0;

    ESP_LOGI(TAG, "ready");
    for (;;) {
        int b = s_button_event;
        s_button_event = 0;
        if (b) handle_event(b);

        finder_poll(&s_view);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_nrf >= 100) {
            last_nrf = now;
            nrf_aux_set_channel(s_view.channel);
            s_view.nrf_ok = nrf_aux_ok();
            s_view.nrf_duty = nrf_aux_duty();
        }
        if (now - last_draw >= 100) {
            last_draw = now;
            wnm_clock_poll(&s_clock);
            st7735_draw_finder(&s_view, &s_clock, s_page);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
