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
static ui_state_t s_ui;
static uint32_t s_btn_ready_ms;

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        int b = st7735_button();
        if (b) s_button_event = b;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void ssid_enc_cycle(void)
{
    if (s_ui.ssid_enc == SSID_ENC_UNKNOWN) s_ui.ssid_enc = SSID_ENC_UTF8;
    else if (s_ui.ssid_enc == SSID_ENC_UTF8) s_ui.ssid_enc = SSID_ENC_GBK;
    else s_ui.ssid_enc = SSID_ENC_UNKNOWN;
    finder_toast("SSID %s",
                 s_ui.ssid_enc == SSID_ENC_UTF8 ? "UTF8" :
                 s_ui.ssid_enc == SSID_ENC_GBK ? "GBK" : "AUTO");
}

static void ap_refresh(void)
{
    s_ui.ap_count = finder_snapshot_aps(s_ui.aps, FINDER_AP_MAX);
    if (s_ui.ap_count <= 0) {
        s_ui.ap_sel = 0;
        s_ui.ap_scroll = 0;
        return;
    }
    if (s_ui.ap_sel >= s_ui.ap_count) s_ui.ap_sel = s_ui.ap_count - 1;
    if (s_ui.ap_sel < 0) s_ui.ap_sel = 0;
    if (s_ui.ap_sel < s_ui.ap_scroll) s_ui.ap_scroll = s_ui.ap_sel;
    if (s_ui.ap_sel >= s_ui.ap_scroll + UI_AP_ROWS)
        s_ui.ap_scroll = s_ui.ap_sel - UI_AP_ROWS + 1;
}

static void ap_list_open(void)
{
    s_ui.ap_sel = 0;
    s_ui.ap_scroll = 0;
    s_ui.sta_sel = 0;
    s_ui.sta_scroll = 0;
    s_ui.sta_count = 0;
    ap_refresh();
    s_ui.page = UI_PAGE_AP_LIST;
}

static void ap_move(int dir)
{
    if (s_ui.ap_count <= 0) return;
    s_ui.ap_sel = (s_ui.ap_sel + dir + s_ui.ap_count) % s_ui.ap_count;
    if (s_ui.ap_sel < s_ui.ap_scroll) s_ui.ap_scroll = s_ui.ap_sel;
    if (s_ui.ap_sel >= s_ui.ap_scroll + UI_AP_ROWS)
        s_ui.ap_scroll = s_ui.ap_sel - UI_AP_ROWS + 1;
}

static void ap_sta_refresh(void)
{
    if (s_ui.ap_sel < 0 || s_ui.ap_sel >= s_ui.ap_count) {
        s_ui.sta_count = 0;
        s_ui.sta_sel = 0;
        s_ui.sta_scroll = 0;
        return;
    }
    s_ui.sta_count = finder_snapshot_stas(s_ui.aps[s_ui.ap_sel].bssid,
                                          s_ui.stas, FINDER_STA_MAX);
    if (s_ui.sta_sel >= s_ui.sta_count)
        s_ui.sta_sel = s_ui.sta_count ? s_ui.sta_count - 1 : 0;
    if (s_ui.sta_sel < 0) s_ui.sta_sel = 0;
    if (s_ui.sta_sel < s_ui.sta_scroll) s_ui.sta_scroll = s_ui.sta_sel;
    if (s_ui.sta_sel >= s_ui.sta_scroll + UI_STA_ROWS)
        s_ui.sta_scroll = s_ui.sta_sel - UI_STA_ROWS + 1;
}

static void sta_move(int dir)
{
    if (s_ui.sta_count <= 0) return;
    s_ui.sta_sel = (s_ui.sta_sel + dir + s_ui.sta_count) % s_ui.sta_count;
    if (s_ui.sta_sel < s_ui.sta_scroll) s_ui.sta_scroll = s_ui.sta_sel;
    if (s_ui.sta_sel >= s_ui.sta_scroll + UI_STA_ROWS)
        s_ui.sta_scroll = s_ui.sta_sel - UI_STA_ROWS + 1;
}

static void menu_execute(void)
{
    switch (s_ui.menu_sel) {
    case UI_MENU_LEARN:
        finder_start_learn();
        s_ui.page = UI_PAGE_FIND;
        break;
    case UI_MENU_CLEAR:
        finder_clear_target();
        s_ui.page = UI_PAGE_FIND;
        break;
    case UI_MENU_PEAK:
        finder_clear_peak();
        s_ui.page = UI_PAGE_FIND;
        break;
    case UI_MENU_CHAN:
        if (s_view.have_target) finder_toggle_lock();
        else finder_next_channel();
        s_ui.page = UI_PAGE_FIND;
        break;
    case UI_MENU_SSID:
        ap_list_open();
        break;
    case UI_MENU_RFSCAN:
        nrf_aux_scan(true);
        s_ui.page = UI_PAGE_RF_SCAN;
        break;
    case UI_MENU_INFO:
        s_ui.page = UI_PAGE_INFO;
        break;
    case UI_MENU_REBOOT:
        esp_restart();
        break;
    default:
        break;
    }
}

static void handle_event(int b)
{
    if (s_ui.page == UI_PAGE_RF_SCAN) {
        if (b == 3) {
            nrf_aux_scan(true); /* restart / clear peaks */
        } else if (b == 4 || b == 14) {
            nrf_aux_scan(false);
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_AP_LIST) {
        if (b == 1) ap_move(1);
        else if (b == 2) ap_move(-1);
        else if (b == 13) ssid_enc_cycle();
        else if (b == 3) {
            if (s_ui.ap_count > 0) s_ui.page = UI_PAGE_AP_DETAIL;
        } else if (b == 4 || b == 14) {
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_AP_DETAIL) {
        if (b == 1) ap_move(1);
        else if (b == 2) ap_move(-1);
        else if (b == 13) ssid_enc_cycle();
        else if (b == 3) {
            ap_sta_refresh();
            s_ui.page = UI_PAGE_AP_STAS;
        } else if (b == 4 || b == 14) {
            s_ui.page = UI_PAGE_AP_LIST;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_AP_STAS) {
        if (b == 1) sta_move(1);
        else if (b == 2) sta_move(-1);
        else if (b == 13) ssid_enc_cycle();
        else if (b == 3) {
            if (s_ui.sta_count > 0 && s_ui.sta_sel >= 0 &&
                s_ui.sta_sel < s_ui.sta_count) {
                finder_set_target_temp(s_ui.stas[s_ui.sta_sel].mac,
                                       "AP list",
                                       s_ui.stas[s_ui.sta_sel].channel);
                s_ui.page = UI_PAGE_FIND;
            }
        } else if (b == 4 || b == 14) {
            s_ui.page = UI_PAGE_AP_DETAIL;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_MENU) {
        if (b == 1) {
            s_ui.menu_sel = (s_ui.menu_sel + 1) % UI_MENU_COUNT;
        } else if (b == 2) {
            s_ui.menu_sel = (s_ui.menu_sel + UI_MENU_COUNT - 1) % UI_MENU_COUNT;
        } else if (b == 3) {
            menu_execute();
        } else if (b == 4 || b == 14) {
            s_ui.page = UI_PAGE_FIND;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_SSID) {
        if (b == 3) {
            size_t n = strlen(s_view.ssid);
            bool u8 = s_view.ssid_nonascii &&
                      ssid_utf8_valid((const uint8_t *)s_view.ssid, n);
            bool gbk = s_view.ssid_nonascii &&
                       ssid_gbk_valid((const uint8_t *)s_view.ssid, n);
            if (u8 && gbk) {
                ssid_enc_t cur = ssid_detect((const uint8_t *)s_view.ssid, n);
                s_ui.ssid_enc = (cur == SSID_ENC_UTF8) ? SSID_ENC_GBK : SSID_ENC_UTF8;
            }
        } else if (b == 4 || b == 14) {
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_INFO) {
        if (b == 4 || b == 14) s_ui.page = UI_PAGE_MENU;
        return;
    }

    /* FINDER */
    switch (b) {
    case 1: finder_next_candidate(); break;
    case 2: finder_prev_candidate(); break;
    case 3:
        if (s_view.have_target) finder_toggle_lock();
        else finder_start_learn();
        break;
    case 4:
        s_ui.menu_sel = 0;
        s_ui.page = UI_PAGE_MENU;
        break;
    case 11: finder_next_channel(); break;
    case 12: finder_prev_channel(); break;
    case 13: finder_start_learn(); break;
    case 14: finder_clear_target(); break;
    default: break;
    }
}

static void print_help(void)
{
    ESP_LOGI("cli", "commands:");
    ESP_LOGI("cli", "  mac AA:BB:CC:DD:EE:FF  set target");
    ESP_LOGI("cli", "  clear                 remove target");
    ESP_LOGI("cli", "  learn                 start 5s learning");
    ESP_LOGI("cli", "  next                  select next candidate");
    ESP_LOGI("cli", "  prev                  select previous candidate");
    ESP_LOGI("cli", "  lock                  toggle channel lock");
    ESP_LOGI("cli", "  chan                  manual next channel");
    ESP_LOGI("cli", "  chprev                manual previous channel");
    ESP_LOGI("cli", "  peak                  clear peak");
    ESP_LOGI("cli", "  list                  print recent candidates with AP/SSID");
    ESP_LOGI("cli", "  stas                  group candidates by AP (BSSID/SSID)");
    ESP_LOGI("cli", "  bss                   print recent BSSID/SSID");
    ESP_LOGI("cli", "  clock                 print WNM time / time zone");
    ESP_LOGI("cli", "  wnm                   print time-element counters");
    ESP_LOGI("cli", "  tz CST-8              manual POSIX time zone fallback");
    ESP_LOGI("cli", "  timemode auto|hostapd|standard   Time Value interpretation");
    ESP_LOGI("cli", "  timeclear             erase the persisted UTC/TSF anchor");
    ESP_LOGI("cli", "  ui ap|detail|stas|rf|find  jump to a UI page (debug)");
    ESP_LOGI("cli", "  rfdata                dump NRF scan peak/avg/last");
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
    ESP_LOGI("cli", "clock=%04u-%02u-%02u %02u:%02u:%02u tz=%s off=%+ld age=%lu src=%s mode=%s",
             s_clock.year, s_clock.month, s_clock.day, s_clock.hour, s_clock.min,
             s_clock.sec, s_clock.tz_name[0] ? s_clock.tz_name : "UTC",
             (long)s_clock.offset_sec, (unsigned long)s_clock.age_ms, src,
             s_clock.persisted ? "persist" :
             (s_clock.tsf_mode ? "tsf" : "direct"));
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
    ESP_LOGI("cli", "wifi=%d target=%s rssi=%d fps=%lu ch=%u mode=%d state=%d cand=%lu pkt=%lu drop=%lu nrf=%d/%u%% csn=%d heap=%lu",
             s_view.wifi_ok, mac, s_view.have_rssi ? s_view.rssi : -100,
             (unsigned long)s_view.fps, s_view.channel, s_view.chan_mode,
             s_view.link, (unsigned long)s_view.cand_count,
             (unsigned long)s_view.packets, (unsigned long)s_view.drops,
             s_view.nrf_ok, s_view.nrf_duty, s_view.nrf_csn,
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI("cli", "vendor=%s bssid=%s ssid=%s peak=%d ap_load=%u%%/%u",
             s_view.vendor[0] ? s_view.vendor : "--", bssid,
             s_view.have_ssid ? s_view.ssid : "--", s_view.peak,
             s_view.have_ap_load ? (unsigned)((s_view.ap_load * 100u + 127u) / 255u) : 0,
             s_view.ap_sta_count);
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
    } else if (!strcmp(line, "prev")) {
        finder_prev_candidate();
    } else if (!strcmp(line, "lock")) {
        finder_toggle_lock();
    } else if (!strcmp(line, "chan")) {
        finder_next_channel();
    } else if (!strcmp(line, "chprev")) {
        finder_prev_channel();
    } else if (!strcmp(line, "peak")) {
        finder_clear_peak();
    } else if (!strcmp(line, "list") || !strcmp(line, "cand")) {
        finder_print_candidates();
    } else if (!strcmp(line, "stas") || !strcmp(line, "byap")) {
        finder_print_stas_by_ap();
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
    } else if (!strcmp(line, "rfdata")) {
        nrf_scan_ch_t sc[NRF_SCAN_CHANNELS];
        nrf_aux_scan_get(sc, NRF_SCAN_CHANNELS);
        ESP_LOGI("cli", "rf ok=%d csn=%d scan=%d cycles=%lu",
                 nrf_aux_ok(), nrf_aux_csn(), nrf_aux_scan_active(),
                 (unsigned long)nrf_aux_scan_cycles());
        for (int i = 0; i < NRF_SCAN_CHANNELS; i++) {
            ESP_LOGI("cli", "ch %2d  peak %3u  avg %3u  last %3u",
                     i + 1, sc[i].peak, sc[i].avg, sc[i].last);
        }
    } else if (!strncmp(line, "ui ", 3)) {
        const char *arg = line + 3;
        if (strcmp(arg, "rf") != 0) nrf_aux_scan(false);
        if (!strcmp(arg, "ap")) ap_list_open();
        else if (!strcmp(arg, "detail")) {
            if (s_ui.ap_count > 0) s_ui.page = UI_PAGE_AP_DETAIL;
        } else if (!strcmp(arg, "stas")) {
            ap_sta_refresh();
            s_ui.page = UI_PAGE_AP_STAS;
        } else if (!strcmp(arg, "rf")) {
            nrf_aux_scan(true);
            s_ui.page = UI_PAGE_RF_SCAN;
        } else if (!strcmp(arg, "menu")) {
            s_ui.page = UI_PAGE_MENU;
        } else if (!strcmp(arg, "find")) {
            s_ui.page = UI_PAGE_FIND;
        }
    } else if (!strcmp(line, "timeclear")) {
        wnm_clock_clear_anchor();
    } else if (!strncmp(line, "timemode ", 9)) {
        wnm_clock_set_mode(line + 9);
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
    xTaskCreatePinnedToCore(console_task, "console", 8192, NULL, 2, NULL, 1);
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
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.page = UI_PAGE_FIND;
    s_ui.ssid_enc = SSID_ENC_UNKNOWN;
    st7735_draw_finder(&s_view, &s_clock, &s_ui);

    nrf_aux_start();
    xTaskCreatePinnedToCore(button_task, "buttons", 2048, NULL, 2, NULL, 1);
    console_start();

    uint32_t last_draw = 0;
    uint32_t last_nrf = 0;
    s_btn_ready_ms = (uint32_t)(esp_timer_get_time() / 1000) + 1500;

    ESP_LOGI(TAG, "ready");
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int b = s_button_event;
        s_button_event = 0;
        if (b && (int32_t)(now - s_btn_ready_ms) >= 0) handle_event(b);

        finder_poll(&s_view);

        if (now - last_nrf >= 100) {
            last_nrf = now;
            nrf_aux_set_channel(s_view.channel);
            s_view.nrf_ok = nrf_aux_ok();
            s_view.nrf_csn = (int8_t)nrf_aux_csn();
            s_view.nrf_duty = nrf_aux_duty();
        }
        if (now - last_draw >= 100) {
            last_draw = now;
            wnm_clock_poll(&s_clock);
            st7735_draw_finder(&s_view, &s_clock, &s_ui);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
