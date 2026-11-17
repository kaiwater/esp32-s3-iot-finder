#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "finder.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nrf_aux.h"
#include "st7735.h"
#include "wnm_clock.h"

static const char *TAG = "iot-finder";
static volatile int s_button_event;
static portMUX_TYPE s_btn_mux = portMUX_INITIALIZER_UNLOCKED;

static void set_button_event(int b)
{
    portENTER_CRITICAL(&s_btn_mux);
    s_button_event = b;
    portEXIT_CRITICAL(&s_btn_mux);
}

static int take_button_event(void)
{
    portENTER_CRITICAL(&s_btn_mux);
    int b = s_button_event;
    s_button_event = 0;
    portEXIT_CRITICAL(&s_btn_mux);
    return b;
}
static finder_view_t s_view;
static wnm_clock_view_t s_clock;
static ui_state_t s_ui;
static uint32_t s_btn_ready_ms;
static uint32_t s_pro_printed;
static void pro_print_list(void);
static volatile uint32_t s_usb_rx_bytes;
static volatile uint32_t s_usb_rx_lines;
static bool s_keypad;

static void button_task(void *arg)
{
    (void)arg;
    for (;;) {
        int b = st7735_button();
        if (b) set_button_event(b);
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
        nrf_aux_mw(false);
        nrf_aux_pro(false);
        finder_wifi_pause();
        nrf_aux_scan(true);
        s_ui.page = UI_PAGE_RF_SCAN;
        break;
    case UI_MENU_MW:
        nrf_aux_scan(false);
        nrf_aux_pro(false);
        finder_wifi_pause();
        nrf_aux_mw(true);
        s_ui.page = UI_PAGE_MW;
        break;
    case UI_MENU_RFPRO:
        nrf_aux_scan(false);
        nrf_aux_mw(false);
        finder_wifi_pause();
        nrf_aux_pro(true);
        s_pro_printed = 0;
        s_ui.pro_view = 0;
        s_ui.page = UI_PAGE_RF_PRO;
        ESP_LOGI("pro", "start: 2400..2525MHz step 1MHz, S=sweep");
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
    if (b == 14) {
        int t = st7735_next_theme();
        finder_toast("THEME %d %s", t + 1, st7735_theme_name());
        return;
    }

    if (s_ui.page == UI_PAGE_COLOR_TEST) {
        if (b == 4) s_ui.page = UI_PAGE_FIND;
        return;
    }

    if (s_ui.page == UI_PAGE_RF_SCAN) {
        if (b == 3) {
            s_ui.help_back = UI_PAGE_RF_SCAN;
            s_ui.page = UI_PAGE_RF_HELP;
        } else if (b == 13) {
            nrf_aux_scan(true); /* long #: restart / clear peaks */
        } else if (b == 4 || b == 14) {
            nrf_aux_scan(false);
            finder_wifi_resume();
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_RF_PRO) {
        if (b == 3) {
            s_ui.pro_view = (s_ui.pro_view + 1) % 3;
            ESP_LOGI("pro", "view %s",
                     s_ui.pro_view == 0 ? "ACTIVITY" :
                     (s_ui.pro_view == 1 ? "PEAK" : "WATERFALL"));
            pro_print_list();
        } else if (b == 13) {
            nrf_aux_pro_reset();
            s_pro_printed = 0;
        } else if (b == 4 || b == 14) {
            nrf_aux_pro(false);
            pro_print_list();
            finder_wifi_resume();
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_MW) {
        if (b == 3) {
            s_ui.help_back = UI_PAGE_MW;
            s_ui.page = UI_PAGE_RF_HELP;
        } else if (b == 13) {
            nrf_aux_mw_reset();
        } else if (b == 4 || b == 14) {
            nrf_aux_mw(false);
            finder_wifi_resume();
            s_ui.page = UI_PAGE_MENU;
        }
        return;
    }

    if (s_ui.page == UI_PAGE_RF_HELP) {
        if (b == 4 || b == 14 || b == 3) {
            s_ui.page = s_ui.help_back;
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
    ESP_LOGI("cli", "  ui ap|detail|stas|rf|help|color|find  jump to a UI page (debug)");
    ESP_LOGI("cli", "  rfdata                dump NRF scan peak/avg/age/hits");
    ESP_LOGI("cli", "  wifioff / wifion      pause/resume ESP32 Wi-Fi (RF scan test)");
    ESP_LOGI("cli", "  mw                    fixed 2450 MHz observation (NRF CH50)");
    ESP_LOGI("cli", "  mw off | mw reset     back to 13 channels / clear MW stats");
    ESP_LOGI("cli", "  pro                   RF Scan Pro: 126 x 1MHz sweep + PC list");
    ESP_LOGI("cli", "  pro off | reset | list  stop / clear / dump high-energy bands");
    ESP_LOGI("cli", "  btn 1..4 [long]       emulate a short/long K1..K4 press");
    ESP_LOGI("cli", "  page                  print the current UI page");
    ESP_LOGI("cli", "  stream [ms]           periodic RF/status line (0=off, default 1000)");
    ESP_LOGI("cli", "  a/d/s/w or A/D/S/W    press K1..K4 (upper case = long)");
    ESP_LOGI("cli", "  keypad [on|off]       raw keys on USB without Enter (Esc exits)");
    ESP_LOGI("cli", "  fmt [template|reset]  show/set the saved RF line template");
    ESP_LOGI("cli", "      %%S cycles  %%c ch  %%p peak  %%h hits  %%t sweeps");
    ESP_LOGI("cli", "      %%0 hits/10s  %%r rate  %%w window  %%a age");
    ESP_LOGI("cli", "  theme                 cycle the 10 UI palettes");
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
    char csn[24] = "-";
    if (s_view.nrf_ok && s_view.nrf_ok2)
        snprintf(csn, sizeof(csn), "%d+%d", s_view.nrf_csn, s_view.nrf_csn2);
    else if (s_view.nrf_ok)
        snprintf(csn, sizeof(csn), "%d", s_view.nrf_csn);
    ESP_LOGI("cli", "wifi=%d usb=%d target=%s rssi=%d fps=%lu ch=%u mode=%d state=%d cand=%lu pkt=%lu drop=%lu nrf=%d/%u%% csn=%s heap=%lu",
             s_view.wifi_ok, usb_serial_jtag_is_connected(),
             mac, s_view.have_rssi ? s_view.rssi : -100,
             (unsigned long)s_view.fps, s_view.channel, s_view.chan_mode,
             s_view.link, (unsigned long)s_view.cand_count,
             (unsigned long)s_view.packets, (unsigned long)s_view.drops,
             s_view.nrf_ok, s_view.nrf_duty, csn,
             (unsigned long)esp_get_free_heap_size());
    ESP_LOGI("cli", "usb host=%d drv=%d rx=%lu lines=%lu",
             usb_serial_jtag_is_connected(),
             usb_serial_jtag_is_driver_installed(),
             (unsigned long)s_usb_rx_bytes, (unsigned long)s_usb_rx_lines);
    ESP_LOGI("cli", "vendor=%s bssid=%s ssid=%s peak=%d ap_load=%u%%/%u",
             s_view.vendor[0] ? s_view.vendor : "--", bssid,
             s_view.have_ssid ? s_view.ssid : "--", s_view.peak,
             s_view.have_ap_load ? (unsigned)((s_view.ap_load * 100u + 127u) / 255u) : 0,
             s_view.ap_sta_count);
    print_clock();
}

#define RF_FMT_DEFAULT \
    "rf S=%S ch=%c pk=%p hit=%h/%t h10=%0  [rate=%r/s] win=%ws age=%as"

static char s_rf_fmt[120] = RF_FMT_DEFAULT;

static uint32_t s_stream_ms = 1000;
static uint32_t s_stream_next;

static void config_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("finder", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, key, val) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

static void config_save_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    if (nvs_open("finder", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u32(h, key, val) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
}

static void config_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return;

    nvs_handle_t h;
    if (nvs_open("finder", NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(s_rf_fmt);
    if (nvs_get_str(h, "rf_fmt", s_rf_fmt, &len) != ESP_OK)
        snprintf(s_rf_fmt, sizeof(s_rf_fmt), "%s", RF_FMT_DEFAULT);

    uint32_t ms = 1000;
    if (nvs_get_u32(h, "stream_ms", &ms) != ESP_OK) ms = 1000;
    s_stream_ms = ms;
    nvs_close(h);
}

static void stream_set(uint32_t ms)
{
    s_stream_ms = ms;
    s_stream_next = (uint32_t)(esp_timer_get_time() / 1000) + ms;
}

typedef struct {
    bool mw;          /* 2450 MHz fixed mode */
    int ch;           /* 0-based scan channel, -1 when none */
    uint32_t cycles;
    uint32_t win;
    uint8_t peak;
    uint8_t avg;
    uint32_t hits;
    uint32_t total;
    uint16_t h10;
    uint32_t age_s;
} rf_line_t;

/* Expand the user template for one RF sample.  Unknown escapes are kept
 * literally so a typo is visible instead of silently dropped. */
static void rf_format_line(char *out, size_t cap, const rf_line_t *v)
{
    size_t o = 0;
    uint32_t rate10 = 0;
    if (v->win && v->h10) rate10 = (uint32_t)v->h10 * 10u / v->win;

    for (const char *p = s_rf_fmt; *p && o + 1 < cap; p++) {
        if (*p != '%') {
            out[o++] = *p;
            continue;
        }
        p++;
        if (!*p) break;

        char fld[24];
        switch (*p) {
        case '%': snprintf(fld, sizeof(fld), "%%"); break;
        case 'S': snprintf(fld, sizeof(fld), "%lu",
                           (unsigned long)v->cycles); break;
        case 'c':
            if (v->mw) snprintf(fld, sizeof(fld), "MW");
            else if (v->ch < 0) snprintf(fld, sizeof(fld), "--");
            else snprintf(fld, sizeof(fld), "%d", v->ch + 1);
            break;
        case 'p': snprintf(fld, sizeof(fld), "%u", v->peak); break;
        case 'h': snprintf(fld, sizeof(fld), "%lu", (unsigned long)v->hits); break;
        case 't': snprintf(fld, sizeof(fld), "%lu", (unsigned long)v->total); break;
        case '0': snprintf(fld, sizeof(fld), "%u", v->h10); break;
        case 'r': snprintf(fld, sizeof(fld), "%lu.%lu",
                           (unsigned long)(rate10 / 10),
                           (unsigned long)(rate10 % 10)); break;
        case 'w': snprintf(fld, sizeof(fld), "%lu", (unsigned long)v->win); break;
        case 'a': snprintf(fld, sizeof(fld), "%lu", (unsigned long)v->age_s); break;
        default:
            fld[0] = '%';
            fld[1] = *p;
            fld[2] = 0;
            break;
        }

        size_t l = strlen(fld);
        if (o + l >= cap) l = cap - o - 1;
        memcpy(out + o, fld, l);
        o += l;
    }
    out[o] = 0;
}

static void pro_print_spectrum(uint32_t cycles)
{
    static nrf_pro_ch_t d[NRF_PRO_CHANNELS];
    nrf_aux_pro_get(d, NRF_PRO_CHANNELS);

    int view = s_ui.pro_view;
    if (view < 0 || view > 2) view = 0;
    const char *tag = view == 1 ? "pk" : (view == 0 ? "act" : "wf");
    char line[160];
    int o = snprintf(line, sizeof(line), "pro S=%lu %s 2400:",
                     (unsigned long)cycles, tag);
    for (int i = 0; i < NRF_PRO_CHANNELS && o + 1 < (int)sizeof(line); i++) {
        int v = view == 1 ? d[i].peak : (view == 0 ? d[i].heat : d[i].last);
        int lv = v / 10;
        if (lv > 9) lv = 9;
        line[o++] = (char)('0' + lv);
    }
    line[o] = 0;
    ESP_LOGI("pro", "%s", line);
}

static void pro_print_list(void)
{
    static nrf_pro_ch_t d[NRF_PRO_CHANNELS];
    nrf_aux_pro_get(d, NRF_PRO_CHANNELS);

    int top = 0;
    for (int i = 1; i < NRF_PRO_CHANNELS; i++)
        if (d[i].peak > d[top].peak) top = i;
    if (d[top].peak)
        ESP_LOGI("pro", "top %uMHz pk=%u%%", 2400 + top, d[top].peak);

    int bands = 0;
    int start = -1;
    for (int i = 0; i <= NRF_PRO_CHANNELS; i++) {
        bool hot = i < NRF_PRO_CHANNELS && d[i].peak >= 50;
        if (hot && start < 0) start = i;
        if (!hot && start >= 0) {
            int end = i - 1;
            uint8_t pk = 0;
            unsigned long hits = 0, sweeps = 0;
            for (int j = start; j <= end; j++) {
                if (d[j].peak > pk) pk = d[j].peak;
                hits += d[j].hits;
                sweeps += d[j].sweeps;
            }
            ESP_LOGI("pro", "hot %u-%uMHz pk=%u%% hit=%lu/%lu",
                     2400 + start, 2400 + end, pk, hits, sweeps);
            bands++;
            start = -1;
        }
    }
    if (!bands) ESP_LOGI("pro", "hot: none (peak < 50%%)");
}

/* Compact one-line telemetry for the PC, independent of what the screen
 * shows.  RF SCAN data wins when the NRF scan is running. */
static void stream_emit(void)
{
    if (nrf_aux_pro_active()) {
        static nrf_pro_ch_t d[NRF_PRO_CHANNELS];
        nrf_aux_pro_get(d, NRF_PRO_CHANNELS);
        int view = s_ui.pro_view;
        if (view < 0 || view > 2) view = 0;
        const char *tag = view == 1 ? "pk" : (view == 0 ? "act" : "wf");
        int top = 0;
        for (int i = 1; i < NRF_PRO_CHANNELS; i++) {
            uint8_t a = view == 1 ? d[i].peak :
                        (view == 0 ? d[i].heat : d[i].last);
            uint8_t b = view == 1 ? d[top].peak :
                        (view == 0 ? d[top].heat : d[top].last);
            if (a > b) top = i;
        }
        uint8_t val = view == 1 ? d[top].peak :
                      (view == 0 ? d[top].heat : d[top].last);
        ESP_LOGI("stream", "pro S=%lu %s top=%uMHz %s=%u%%",
                 (unsigned long)nrf_aux_pro_cycles(), tag,
                 2400 + top, tag, val);
        return;
    }

    if (nrf_aux_mw_active()) {
        nrf_mw_t mw;
        nrf_aux_mw_get(&mw);
        uint32_t win = nrf_aux_mw_elapsed_ms() / 1000;
        if (win > 10) win = 10;
        rf_line_t v = {
            .mw = true,
            .ch = -1,
            .cycles = mw.sweeps,
            .win = win,
            .peak = mw.peak,
            .avg = mw.avg,
            .hits = mw.hits,
            .total = mw.sweeps,
            .h10 = mw.hits_10s,
            .age_s = mw.peak_age_ms / 1000,
        };
        char line[160];
        rf_format_line(line, sizeof(line), &v);
        ESP_LOGI("stream", "%s", line);
        return;
    }

    if (nrf_aux_scan_active()) {
        nrf_scan_ch_t sc[NRF_SCAN_CHANNELS];
        nrf_aux_scan_get(sc, NRF_SCAN_CHANNELS);

        int ch = st7735_rf_shown_channel();
        if (ch < 0 || ch >= NRF_SCAN_CHANNELS || sc[ch].peak == 0) {
            ch = -1;
            for (int i = 0; i < NRF_SCAN_CHANNELS; i++) {
                if (ch < 0 || sc[i].peak > sc[ch].peak) ch = i;
            }
            if (ch >= 0 && sc[ch].peak == 0) ch = -1;
        }

        uint32_t win = nrf_aux_scan_elapsed_ms() / 1000;
        if (win > 10) win = 10;

        rf_line_t v = {
            .mw = false,
            .ch = ch,
            .cycles = nrf_aux_scan_cycles(),
            .win = win,
        };
        if (ch >= 0) {
            v.peak = sc[ch].peak;
            v.avg = sc[ch].avg;
            v.hits = sc[ch].hits;
            v.total = sc[ch].sweeps;
            v.h10 = sc[ch].hits_10s;
            v.age_s = sc[ch].peak_age_ms / 1000;
        }
        char line[160];
        rf_format_line(line, sizeof(line), &v);
        ESP_LOGI("stream", "%s", line);
        return;
    }

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
    ESP_LOGI("stream",
             "find target=%s bssid=%s rssi=%d fps=%lu ch=%u mode=%d state=%d cand=%lu pkt=%lu",
             mac, bssid, s_view.have_rssi ? s_view.rssi : -100,
             (unsigned long)s_view.fps, s_view.channel, s_view.chan_mode,
             s_view.link, (unsigned long)s_view.cand_count,
             (unsigned long)s_view.packets);
}

static const char *page_name(int page)
{
    switch (page) {
    case UI_PAGE_FIND: return "find";
    case UI_PAGE_MENU: return "menu";
    case UI_PAGE_INFO: return "info";
    case UI_PAGE_SSID: return "ssid";
    case UI_PAGE_AP_LIST: return "ap-list";
    case UI_PAGE_AP_DETAIL: return "ap-detail";
    case UI_PAGE_AP_STAS: return "ap-stas";
    case UI_PAGE_RF_SCAN: return "rf-scan";
    case UI_PAGE_RF_HELP: return "rf-help";
    case UI_PAGE_MW: return "mw";
    case UI_PAGE_RF_PRO: return "rf-pro";
    case UI_PAGE_COLOR_TEST: return "color";
    default: return "--";
    }
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
        char csn[16];
        if (nrf_aux_ok2() && nrf_aux_csn2() >= 0)
            snprintf(csn, sizeof(csn), "%d+%d", nrf_aux_csn(), nrf_aux_csn2());
        else
            snprintf(csn, sizeof(csn), "%d", nrf_aux_csn());
        ESP_LOGI("cli", "rf ok=%d csn=%s scan=%d cycles=%lu",
                 nrf_aux_ok(), csn, nrf_aux_scan_active(),
                 (unsigned long)nrf_aux_scan_cycles());
        for (int i = 0; i < NRF_SCAN_CHANNELS; i++) {
            ESP_LOGI("cli", "ch %2d  peak %3u  avg %3u  last %3u  age %3lus  hit %u/%u  streak %u/%u",
                     i + 1, sc[i].peak, sc[i].avg, sc[i].last,
                     (unsigned long)(sc[i].peak_age_ms / 1000),
                     sc[i].hits, sc[i].sweeps,
                     sc[i].streak, sc[i].max_streak);
        }
    } else if (!strncmp(line, "ui ", 3)) {
        const char *arg = line + 3;
        if (strcmp(arg, "rf") != 0 && strcmp(arg, "help") != 0 &&
            strcmp(arg, "mw") != 0 && strcmp(arg, "pro") != 0) {
            nrf_aux_scan(false);
            nrf_aux_mw(false);
            nrf_aux_pro(false);
            finder_wifi_resume();
        }
        if (!strcmp(arg, "ap")) ap_list_open();
        else if (!strcmp(arg, "detail")) {
            if (s_ui.ap_count > 0) s_ui.page = UI_PAGE_AP_DETAIL;
        } else if (!strcmp(arg, "stas")) {
            ap_sta_refresh();
            s_ui.page = UI_PAGE_AP_STAS;
        } else if (!strcmp(arg, "rf")) {
            nrf_aux_mw(false);
            nrf_aux_pro(false);
            finder_wifi_pause();
            nrf_aux_scan(true);
            s_ui.page = UI_PAGE_RF_SCAN;
        } else if (!strcmp(arg, "mw")) {
            nrf_aux_scan(false);
            nrf_aux_pro(false);
            finder_wifi_pause();
            nrf_aux_mw(true);
            s_ui.page = UI_PAGE_MW;
        } else if (!strcmp(arg, "pro")) {
            nrf_aux_scan(false);
            nrf_aux_mw(false);
            finder_wifi_pause();
            nrf_aux_pro(true);
            s_pro_printed = 0;
            s_ui.pro_view = 0;
            s_ui.page = UI_PAGE_RF_PRO;
            ESP_LOGI("pro", "start: 2400..2525MHz step 1MHz, S=sweep");
        } else if (!strcmp(arg, "help")) {
            nrf_aux_mw(false);
            nrf_aux_pro(false);
            finder_wifi_pause();
            nrf_aux_scan(true);
            s_ui.help_back = UI_PAGE_RF_SCAN;
            s_ui.page = UI_PAGE_RF_HELP;
        } else if (!strcmp(arg, "color")) {
            s_ui.page = UI_PAGE_COLOR_TEST;
        } else if (!strcmp(arg, "menu")) {
            s_ui.page = UI_PAGE_MENU;
        } else if (!strcmp(arg, "find")) {
            s_ui.page = UI_PAGE_FIND;
        }
    } else if (!strncmp(line, "btn ", 4)) {
        int n = 0;
        char mode[8] = {0};
        int got = sscanf(line + 4, "%d %7s", &n, mode);
        if (got >= 1 && n >= 1 && n <= 4) {
            bool lng = got >= 2 &&
                       (mode[0] == 'l' || mode[0] == 'L' || mode[0] == 'h');
            set_button_event(lng ? 10 + n : n);
            ESP_LOGI("cli", "button %d %s", n, lng ? "long" : "short");
        } else {
            ESP_LOGW("cli", "usage: btn 1..4 [long]");
        }
    } else if (!strncmp(line, "stream", 6)) {
        const char *arg = line + 6;
        while (*arg == ' ') arg++;
        uint32_t ms = 1000;
        if (*arg) ms = (uint32_t)strtoul(arg, NULL, 10);
        if (ms && ms < 100) ms = 100;
        if (ms > 60000) ms = 60000;
        stream_set(ms);
        config_save_u32("stream_ms", ms);
        ESP_LOGI("cli", "stream %s (%lu ms, saved)",
                 ms ? "on" : "off", (unsigned long)ms);
    } else if (!strcmp(line, "keypad") || !strcmp(line, "keypad on") ||
               !strcmp(line, "keypad off")) {
        if (!strcmp(line, "keypad on")) s_keypad = true;
        else if (!strcmp(line, "keypad off")) s_keypad = false;
        ESP_LOGI("cli", "keypad %s (a/d/s/w = K1..K4, Esc exits)",
                 s_keypad ? "on" : "off");
    } else if (strlen(line) == 1 &&
               strchr("adswADSW", line[0]) != NULL) {
        char k = (char)tolower((unsigned char)line[0]);
        int btn = k == 'a' ? 1 : k == 'd' ? 2 : k == 's' ? 3 : 4;
        bool lng = isupper((unsigned char)line[0]) != 0;
        set_button_event(lng ? 10 + btn : btn);
        ESP_LOGI("cli", "key '%c' -> K%d %s", line[0], btn,
                 lng ? "long" : "short");
    } else if (!strcmp(line, "fmt")) {
        ESP_LOGI("cli", "rf_fmt=%s", s_rf_fmt);
    } else if (!strncmp(line, "fmt ", 4)) {
        const char *arg = line + 4;
        if (!strcmp(arg, "reset"))
            snprintf(s_rf_fmt, sizeof(s_rf_fmt), "%s", RF_FMT_DEFAULT);
        else
            snprintf(s_rf_fmt, sizeof(s_rf_fmt), "%s", arg);
        config_save_str("rf_fmt", s_rf_fmt);
        ESP_LOGI("cli", "rf_fmt=%s", s_rf_fmt);
    } else if (!strcmp(line, "page")) {
        ESP_LOGI("cli", "page=%s(%d) menu_sel=%d theme=%d wifi=%d",
                 page_name(s_ui.page), (int)s_ui.page, s_ui.menu_sel,
                 st7735_theme(), s_view.wifi_ok);
    } else if (!strcmp(line, "mw")) {
        nrf_aux_scan(false);
        finder_wifi_pause();
        nrf_aux_mw(true);
        s_ui.page = UI_PAGE_MW;
        ESP_LOGI("cli", "MW 2450 on (NRF RF_CH=50)");
    } else if (!strcmp(line, "mw off")) {
        nrf_aux_mw(false);
        if (s_ui.page == UI_PAGE_MW ||
            (s_ui.page == UI_PAGE_RF_HELP && s_ui.help_back == UI_PAGE_MW)) {
            nrf_aux_scan(true);
            s_ui.help_back = UI_PAGE_RF_SCAN;
            s_ui.page = UI_PAGE_RF_SCAN;
        }
        ESP_LOGI("cli", "MW 2450 off, 13-channel scan");
    } else if (!strcmp(line, "mw reset")) {
        nrf_aux_mw_reset();
        ESP_LOGI("cli", "MW 2450 stats reset");
    } else if (!strcmp(line, "pro")) {
        nrf_aux_scan(false);
        nrf_aux_mw(false);
        finder_wifi_pause();
        nrf_aux_pro(true);
        s_pro_printed = 0;
        s_ui.pro_view = 0;
        s_ui.page = UI_PAGE_RF_PRO;
        ESP_LOGI("pro", "start: 2400..2525MHz step 1MHz, S=sweep");
    } else if (!strcmp(line, "pro off")) {
        nrf_aux_pro(false);
        pro_print_list();
        if (s_ui.page == UI_PAGE_RF_PRO) {
            nrf_aux_scan(true);
            s_ui.page = UI_PAGE_RF_SCAN;
        }
        ESP_LOGI("pro", "stopped");
    } else if (!strcmp(line, "pro reset")) {
        nrf_aux_pro_reset();
        s_pro_printed = 0;
        ESP_LOGI("pro", "stats reset");
    } else if (!strcmp(line, "pro list")) {
        pro_print_list();
    } else if (!strcmp(line, "wifioff")) {
        finder_wifi_pause();
    } else if (!strcmp(line, "wifion")) {
        finder_wifi_resume();
    } else if (!strcmp(line, "theme")) {
        int t = st7735_next_theme();
        ESP_LOGI("cli", "theme %d %s", t + 1, st7735_theme_name());
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
    char line[128];
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

static void usb_console_task(void *arg)
{
    (void)arg;
    char line[128];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(200)) != 1) continue;
        s_usb_rx_bytes++;

        if (s_keypad) {
            if (c == 0x1B) { /* Esc leaves key mode */
                s_keypad = false;
                n = 0;
                ESP_LOGI("cli", "keypad off");
                continue;
            }
            if (c == '\r' || c == '\n') continue;
            char k = (char)c;
            char lk = (char)tolower((unsigned char)k);
            int btn = lk == 'a' ? 1 : lk == 'd' ? 2 : lk == 's' ? 3 :
                      lk == 'w' ? 4 : 0;
            if (btn) {
                bool lng = isupper((unsigned char)k) != 0;
                set_button_event(lng ? 10 + btn : btn);
                ESP_LOGI("cli", "keypad '%c' -> K%d %s", k, btn,
                         lng ? "long" : "short");
            }
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                s_usb_rx_lines++;
                ESP_LOGI("cli", "usb> %s", line);
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

static void usb_console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 512,
        .rx_buffer_size = 512,
    };
    esp_err_t err = ESP_OK;
    if (!usb_serial_jtag_is_driver_installed()) {
        err = usb_serial_jtag_driver_install(&cfg);
        /* Move the console VFS to the driver as well.  Mixing the ROM
         * (no-driver) TX path with the driver RX path is what ESP-IDF's
         * own console does not do. */
        if (err == ESP_OK) usb_serial_jtag_vfs_use_driver();
    }
    if (err != ESP_OK) {
        ESP_LOGW("cli", "USB serial console unavailable: %s",
                 esp_err_to_name(err));
        return;
    }
    xTaskCreatePinnedToCore(usb_console_task, "usbcon", 4096, NULL, 2, NULL, 1);
    ESP_LOGI("cli", "USB serial console ready (native USB port)");
    ESP_LOGI("cli", "USB: send 'help', 'status', 'rfdata', 'btn 1..4 [long]'");
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
    s_ui.ssid_enc = SSID_ENC_UNKNOWN;

    /* Boot straight into RF SCAN PRO with Wi-Fi paused, so the NRF sees
     * the environment without the ESP32 radio desensing it.  The default
     * view is the fading ACTIVITY spectrum. */
    nrf_aux_start();
    finder_wifi_pause();
    nrf_aux_pro(true);
    s_ui.pro_view = 0;
    s_pro_printed = 0;
    s_ui.page = UI_PAGE_RF_PRO;
    ESP_LOGI("pro", "boot: 2400..2525MHz step 1MHz, S=sweep");

    /* Saved interval and template live in NVS, so changing them does not
     * need a rebuild.  Default is 1 Hz. */
    config_load();
    stream_set(s_stream_ms ? s_stream_ms : 1000);
    st7735_draw_finder(&s_view, &s_clock, &s_ui);

    xTaskCreatePinnedToCore(button_task, "buttons", 2048, NULL, 2, NULL, 1);
    console_start();
    usb_console_start();

    uint32_t last_draw = 0;
    uint32_t last_nrf = 0;
    s_btn_ready_ms = (uint32_t)(esp_timer_get_time() / 1000) + 1500;

    ESP_LOGI(TAG, "ready");
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        int b = take_button_event();
        if (b && (int32_t)(now - s_btn_ready_ms) >= 0) {
            int before = (int)s_ui.page;
            handle_event(b);
            ESP_LOGI("cli", "btn %d page %d->%d", b, before, (int)s_ui.page);
        }

        finder_poll(&s_view);

        if (now - last_nrf >= 100) {
            last_nrf = now;
            nrf_aux_set_channel(s_view.channel);
            s_view.nrf_ok = nrf_aux_ok();
            s_view.nrf_ok2 = nrf_aux_ok2();
            s_view.nrf_csn = (int8_t)nrf_aux_csn();
            s_view.nrf_csn2 = (int8_t)nrf_aux_csn2();
            s_view.nrf_duty = nrf_aux_duty();
        }
        if (s_stream_ms && (int32_t)(now - s_stream_next) >= 0) {
            s_stream_next += s_stream_ms;
            if ((int32_t)(now - s_stream_next) >= 0)
                s_stream_next = now + s_stream_ms;
            stream_emit();
        }

        if (nrf_aux_pro_active()) {
            uint32_t cyc = nrf_aux_pro_cycles();
            if (cyc != s_pro_printed) {
                s_pro_printed = cyc;
                pro_print_spectrum(cyc);
                if ((cyc % 5) == 0) pro_print_list();
            }
        }

        if (now - last_draw >= 100) {
            last_draw = now;
            wnm_clock_poll(&s_clock);
            st7735_draw_finder(&s_view, &s_clock, &s_ui);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
