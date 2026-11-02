/* ST7735 160x128 IoT finder UI.
 *
 * Layout is deliberately compact: large RSSI, a live bar, peer BSSID/SSID,
 * channel mode and the optional NRF RPD side channel.  Deep-green PHO theme
 * is the default requested for the handheld build.
 */
#include "st7735.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7735.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WIDTH 160
#define HEIGHT 128
#define FRAME_PIXELS (WIDTH * HEIGHT)

#define PIN_SCK 12
#define PIN_MOSI 11
#define PIN_CS 10
#define PIN_DC 9
#define PIN_RST 8
#define PIN_BL 7

#define PIN_BTN_UP 4
#define PIN_BTN_DOWN 5
#define PIN_BTN_OK 6
#define PIN_BTN_BACK 14

#define BTN_LONG_MS 700

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define COL_BG     RGB565(0x00, 0x14, 0x08)
#define COL_TEXT   RGB565(0xD8, 0xFF, 0xE4)
#define COL_ACCENT RGB565(0x38, 0xFF, 0x78)
#define COL_DIM    RGB565(0x18, 0x7A, 0x3C)
#define COL_DARK   RGB565(0x04, 0x24, 0x14)
#define COL_WARN   RGB565(0xFF, 0xD0, 0x20)
#define COL_ORANGE RGB565(0xFF, 0x70, 0x10)
#define COL_RED    RGB565(0xFF, 0x30, 0x30)

static const char *TAG = "st7735";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_fb;

static const uint8_t s_font8[64][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* ! */
    {0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* quote */
    {0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00}, /* # */
    {0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00}, /* $ */
    {0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00}, /* % */
    {0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00}, /* & */
    {0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' */
    {0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00}, /* ( */
    {0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00}, /* ) */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* * */
    {0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00}, /* + */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* , */
    {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00}, /* - */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* . */
    {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00}, /* / */
    {0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00}, /* 0 */
    {0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00}, /* 1 */
    {0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00}, /* 2 */
    {0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00}, /* 3 */
    {0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00}, /* 4 */
    {0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00}, /* 5 */
    {0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00}, /* 6 */
    {0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00}, /* 7 */
    {0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00}, /* 8 */
    {0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00}, /* 9 */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00}, /* : */
    {0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06}, /* ; */
    {0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00}, /* < */
    {0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00}, /* = */
    {0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00}, /* > */
    {0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00}, /* ? */
    {0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00}, /* @ */
    {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00}, /* A */
    {0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00}, /* B */
    {0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00}, /* C */
    {0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00}, /* D */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00}, /* E */
    {0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00}, /* F */
    {0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00}, /* G */
    {0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00}, /* H */
    {0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* I */
    {0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00}, /* J */
    {0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00}, /* K */
    {0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00}, /* L */
    {0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00}, /* M */
    {0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00}, /* N */
    {0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00}, /* O */
    {0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00}, /* P */
    {0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00}, /* Q */
    {0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00}, /* R */
    {0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00}, /* S */
    {0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00}, /* T */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00}, /* U */
    {0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00}, /* V */
    {0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00}, /* W */
    {0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00}, /* X */
    {0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00}, /* Y */
    {0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00}, /* Z */
    {0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00}, /* [ */
    {0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00}, /* backslash */
    {0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00}, /* ] */
    {0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00}, /* ^ */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* _ */
};

static unsigned glyph3x5(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
    case '0': return 075557; case '1': return 026227; case '2': return 071747;
    case '3': return 071717; case '4': return 055711; case '5': return 074717;
    case '6': return 074757; case '7': return 071111; case '8': return 075757;
    case '9': return 075717; case 'A': return 025755; case 'B': return 065656;
    case 'C': return 074447; case 'D': return 065556; case 'E': return 074647;
    case 'F': return 074644; case 'G': return 074557; case 'H': return 055755;
    case 'I': return 072227; case 'J': return 011157; case 'K': return 055655;
    case 'L': return 044447; case 'M': return 057555; case 'N': return 057755;
    case 'O': return 025552; case 'P': return 075744; case 'Q': return 075573;
    case 'R': return 075765; case 'S': return 074717; case 'T': return 072222;
    case 'U': return 055557; case 'V': return 055552; case 'W': return 055575;
    case 'X': return 055255; case 'Y': return 055222; case 'Z': return 071247;
    case '-': return 000700; case '.': return 000002; case '/': return 011244;
    case ':': return 002020; case '+': return 002720; case '!': return 022200;
    case '?': return 071447; case '_': return 000007; case '=': return 007070;
    default: return 0;
    }
}

static void rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > WIDTH) w = WIDTH - x;
    if (y + h > HEIGHT) h = HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) s_fb[yy * WIDTH + xx] = color;
}

static void text3x5(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += 4 * scale) {
        unsigned bits = glyph3x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (bits & (1u << (14 - row * 3 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

/* 2x5 numeric font for the service clock and other low-priority counters.
 * Three pixels per character including the gap, so a HH:MM clock fits in
 * the header without competing with the main RSSI display. */
static unsigned glyph2x5(char c)
{
    switch (c) {
    case '0': return 0x3AB; case '1': return 0x155; case '2': return 0x37B;
    case '3': return 0x377; case '4': return 0x2B5; case '5': return 0x3B7;
    case '6': return 0x3BF; case '7': return 0x355; case '8': return 0x3BB;
    case '9': return 0x3F7; case ':': return 0x044; case '-': return 0x030;
    case '.': return 0x001; case ' ': return 0;
    default: return 0;
    }
}

static void text2x5(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += 3 * scale) {
        unsigned bits = glyph2x5(*s);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 2; col++)
                if (bits & (1u << (9 - row * 2 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}

static void service_clock(char *out, size_t n, bool with_seconds)
{
    uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t h = sec / 3600;
    uint32_t m = (sec / 60) % 60;
    uint32_t s = sec % 60;
    if (h > 99) h = 99;
    if (with_seconds) snprintf(out, n, "%02lu:%02lu:%02lu",
                               (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else snprintf(out, n, "%02lu:%02lu", (unsigned long)h, (unsigned long)m);
}

static void text8(int x, int y, const char *s, int scale, uint16_t color)
{
    for (; *s; s++, x += 8 * scale) {
        char c = *s;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 32 || c > 95) c = '?';
        const uint8_t *g = s_font8[c - 32];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (1u << col))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
    }
}


static void clear_panel(void)
{
    uint32_t pair = ((uint32_t)COL_BG << 16) | COL_BG;
    uint32_t *p = (uint32_t *)s_fb;
    for (int i = 0; i < FRAME_PIXELS / 2; i++) p[i] = pair;
}

static void fit_text(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    if (!n) return;
    for (; src && *src && j + 1 < n; src++) {
        unsigned char c = (unsigned char)*src;
        if (c >= 0x80) {
            if ((c & 0xC0) != 0x80) dst[j++] = '?';
            continue;
        }
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        if (c == '_') c = '-';
        if (c == ',') c = '.';
        if (c == '&') c = '+';
        if (c == '(' || c == ')' || c == '\'' || c == '"') c = ' ';
        if (c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == ':' || c == '/' || c == '+') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '?';
        }
    }
    dst[j] = 0;
}

static uint16_t hot_color(int8_t rssi, bool have)
{
    if (!have) return COL_DIM;
    if (rssi >= -45) return COL_ACCENT;
    if (rssi >= -60) return COL_WARN;
    if (rssi >= -75) return COL_ORANGE;
    return COL_RED;
}

static const char *hot_name(int8_t rssi, bool have)
{
    if (!have) return "COLD";
    if (rssi >= -45) return "HOT";
    if (rssi >= -60) return "WARM";
    if (rssi >= -75) return "COOL";
    return "COLD";
}

static const char *link_name(finder_link_t link)
{
    switch (link) {
    case FINDER_LINK_SEEN: return "SEEN";
    case FINDER_LINK_WEAK: return "WEAK";
    case FINDER_LINK_LOST: return "LOST";
    default: return "NONE";
    }
}

static uint16_t link_color(finder_link_t link)
{
    switch (link) {
    case FINDER_LINK_SEEN: return COL_ACCENT;
    case FINDER_LINK_WEAK: return COL_WARN;
    case FINDER_LINK_LOST: return COL_RED;
    default: return COL_DIM;
    }
}

static const char *mode_name(finder_chan_mode_t mode)
{
    switch (mode) {
    case FINDER_CH_LOCKED: return "LOCK";
    case FINDER_CH_MANUAL: return "MAN";
    default: return "AUTO";
    }
}

static uint16_t mode_color(finder_chan_mode_t mode)
{
    switch (mode) {
    case FINDER_CH_LOCKED: return COL_ACCENT;
    case FINDER_CH_MANUAL: return COL_WARN;
    default: return COL_DIM;
    }
}

static void mac_text(const uint8_t *m, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void age_text(char *out, size_t n, uint32_t ms, bool valid)
{
    if (!valid) {
        snprintf(out, n, "--");
    } else if (ms < 10000) {
        snprintf(out, n, "%lu.%lus", (unsigned long)(ms / 1000),
                 (unsigned long)((ms % 1000) / 100));
    } else if (ms < 100000) {
        snprintf(out, n, "%lus", (unsigned long)(ms / 1000));
    } else {
        snprintf(out, n, ">99s");
    }
}

static void draw_mac_line(const finder_view_t *v)
{
    char mac[18];
    if (v->have_target) {
        mac_text(v->target, mac);
        text3x5(2, 14, mac, 1, COL_TEXT);
        text3x5(132, 14, mode_name(v->chan_mode), 1, mode_color(v->chan_mode));
    } else {
        text3x5(2, 13, "NO TARGET", 2, COL_ACCENT);
        text3x5(132, 14, mode_name(v->chan_mode), 1, mode_color(v->chan_mode));
    }
}

static void draw_finder_page(const finder_view_t *v)
{
    char buf[96], tmp[64];
    bool have_rssi = v->have_rssi;

    clear_panel();

    text3x5(2, 2, "IOT FIND", 2, COL_TEXT);
    text3x5(128, 2, "2.4G", 1, COL_DIM);
    char clk[16];
    service_clock(clk, sizeof(clk), false);
    text3x5(114, 7, "UP", 1, COL_DIM);
    text2x5(124, 7, clk, 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    draw_mac_line(v);

    if (v->have_target) {
        fit_text(tmp, sizeof(tmp), v->vendor);
        if (!tmp[0]) snprintf(tmp, sizeof(tmp), "UNKNOWN");
        tmp[28] = 0;
        text3x5(2, 21, tmp, 1, COL_DIM);
    } else {
        text3x5(2, 21, "K2 HOLD = LEARN 5S", 1, COL_DIM);
    }
    snprintf(tmp, sizeof(tmp), "CH %u", v->channel);
    text3x5(132, 21, tmp, 1, COL_TEXT);

    text3x5(2, 26, "RSSI", 1, COL_DIM);

    if (have_rssi) {
        snprintf(buf, sizeof(buf), "%d", v->rssi);
        text8(4, 31, buf, 3, hot_color(v->rssi, true));
        text3x5(100, 41, "DBM", 2, COL_DIM);
        text3x5(126, 41, hot_name(v->rssi, true), 2, hot_color(v->rssi, true));
    } else {
        text8(4, 31, "--", 3, COL_DIM);
        text3x5(58, 41, "NO SIGNAL", 1, COL_DIM);
        text3x5(126, 41, "COLD", 2, COL_DIM);
    }

    rect(4, 58, 152, 10, COL_DARK);
    if (have_rssi && v->percent) {
        int fill = 152 * (int)v->percent / 100;
        if (fill < 2) fill = 2;
        rect(4, 58, fill, 10, hot_color(v->rssi, true));
    }
    rect(4, 58, 152, 1, COL_DIM);
    rect(4, 67, 152, 1, COL_DIM);
    rect(42, 58, 1, 10, COL_DARK);
    rect(80, 58, 1, 10, COL_DARK);
    rect(118, 58, 1, 10, COL_DARK);

    if (v->peak < -120) snprintf(tmp, sizeof(tmp), "PEAK --");
    else snprintf(tmp, sizeof(tmp), "PEAK %d", v->peak);
    text3x5(4, 72, tmp, 1, COL_TEXT);
    snprintf(tmp, sizeof(tmp), "FPS %lu", (unsigned long)v->fps);
    text3x5(120, 72, tmp, 1, COL_TEXT);

    age_text(tmp, sizeof(tmp), v->age_ms, v->have_rssi);
    snprintf(buf, sizeof(buf), "LAST %s", tmp);
    text3x5(4, 79, buf, 1, COL_DIM);
    text3x5(132, 79, link_name(v->link), 1, link_color(v->link));

    if (v->have_bssid) {
        mac_text(v->bssid, tmp);
        snprintf(buf, sizeof(buf), "BSSID %s", tmp);
    } else {
        snprintf(buf, sizeof(buf), "BSSID --");
    }
    text3x5(4, 87, buf, 1, COL_DIM);

    if (v->have_ssid) {
        fit_text(tmp, sizeof(tmp), v->ssid);
        snprintf(buf, sizeof(buf), "SSID %s", tmp);
    } else {
        snprintf(buf, sizeof(buf), "SSID --");
    }
    text3x5(4, 94, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "CH %u %s", v->channel, mode_name(v->chan_mode));
    text3x5(4, 101, buf, 1, COL_TEXT);
    snprintf(buf, sizeof(buf), "CAND %lu", (unsigned long)v->cand_count);
    text3x5(72, 101, buf, 1, COL_DIM);
    if (v->nrf_ok) snprintf(buf, sizeof(buf), "RF %u%%", v->nrf_duty);
    else snprintf(buf, sizeof(buf), "RF --");
    text3x5(128, 101, buf, 1, COL_DIM);

    rect(0, 110, WIDTH, 1, COL_DARK);

    if (v->toast[0]) {
        fit_text(tmp, sizeof(tmp), v->toast);
        tmp[38] = 0;
        text3x5(2, 114, tmp, 1, COL_ACCENT);
    } else if (v->learning) {
        char best[18] = "--";
        if (v->have_best) mac_text(v->best_mac, best);
        snprintf(buf, sizeof(buf), "LEARN %lu.%lus  %s", (unsigned long)(v->learn_left_ms / 1000),
                 (unsigned long)((v->learn_left_ms % 1000) / 100), best);
        text3x5(2, 114, buf, 1, COL_WARN);
    } else if (!v->have_target) {
        text3x5(2, 114, "K2 LEARN  K3 NEXT", 1, COL_DIM);
    } else {
        text3x5(2, 114, "K1 LOCK  K2 LEARN  K3 NEXT", 1, COL_DIM);
    }

    if (v->learning) {
        text3x5(2, 121, "HOLD STILL  K2 = RESTART", 1, COL_DIM);
    } else if (v->have_target) {
        text3x5(2, 121, "K3 HOLD CLEAR  K4 INFO", 1, COL_DIM);
    } else {
        text3x5(2, 121, "K4 INFO  HOLD K4 RESET", 1, COL_DIM);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_info_page(const finder_view_t *v)
{
    char buf[96], tmp[64], mac[18], clk[16], pk[8];

    clear_panel();

    text3x5(2, 2, "INFO", 2, COL_ACCENT);
    text3x5(96, 3, "IOT FIND", 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (v->have_target) {
        mac_text(v->target, mac);
        snprintf(buf, sizeof(buf), "TARGET %s", mac);
    } else {
        snprintf(buf, sizeof(buf), "TARGET --");
    }
    text3x5(2, 17, buf, 1, COL_TEXT);

    fit_text(tmp, sizeof(tmp), v->vendor);
    if (!tmp[0]) snprintf(tmp, sizeof(tmp), "--");
    tmp[31] = 0;
    text3x5(2, 24, "VENDOR", 1, COL_DIM);
    text3x5(32, 24, tmp, 1, COL_TEXT);

    if (v->have_bssid) {
        mac_text(v->bssid, mac);
        snprintf(buf, sizeof(buf), "BSSID %s", mac);
    } else {
        snprintf(buf, sizeof(buf), "BSSID --");
    }
    text3x5(2, 31, buf, 1, COL_TEXT);

    if (v->have_ssid) {
        fit_text(tmp, sizeof(tmp), v->ssid);
        snprintf(buf, sizeof(buf), "SSID %s", tmp);
    } else {
        snprintf(buf, sizeof(buf), "SSID --");
    }
    text3x5(2, 38, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "CH %u  %s  TARGET CH %u", v->channel,
             mode_name(v->chan_mode), v->target_channel);
    text3x5(2, 45, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "CAND %lu  PKT %lu  DROP %lu",
             (unsigned long)v->cand_count, (unsigned long)v->packets,
             (unsigned long)v->drops);
    text3x5(2, 52, buf, 1, COL_TEXT);

    if (v->peak < -120) snprintf(pk, sizeof(pk), "--");
    else snprintf(pk, sizeof(pk), "%d", v->peak);
    snprintf(buf, sizeof(buf), "FPS %lu  PEAK %s  AGE %luMS",
             (unsigned long)v->fps, pk, (unsigned long)v->age_ms);
    text3x5(2, 59, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "LINK %s  RF %s %u%%", link_name(v->link),
             v->nrf_ok ? "OK" : "--", v->nrf_duty);
    text3x5(2, 66, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "WIFI %s  TARGET %s", v->wifi_ok ? "OK" : "FAIL",
             v->have_target ? "SET" : "NONE");
    text3x5(2, 73, buf, 1, COL_TEXT);

    text3x5(2, 82, "K1 SHORT = LOCK / HOP", 1, COL_DIM);
    text3x5(2, 89, "K1 HOLD  = NEXT CHANNEL", 1, COL_DIM);
    text3x5(2, 96, "K2 HOLD  = LEARN 5S", 1, COL_DIM);
    text3x5(2, 103, "K3 SHORT = NEXT CANDIDATE", 1, COL_DIM);
    text3x5(2, 110, "K3 HOLD  = CLEAR TARGET", 1, COL_DIM);

    service_clock(clk, sizeof(clk), true);
    text3x5(2, 119, "UP", 1, COL_DIM);
    text2x5(14, 119, clk, 1, COL_ACCENT);
    text3x5(48, 119, "K4 BACK", 1, COL_TEXT);
    text3x5(88, 119, "HOLD=RST", 1, COL_DIM);

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

void st7735_draw_finder(const finder_view_t *v, int page)
{
    if (!s_panel || !s_fb || !v) return;
    if (page == 0) draw_finder_page(v);
    else draw_info_page(v);
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

void st7735_init(void)
{
    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FRAME_PIXELS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 27 * 1000 * 1000,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) abort();
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_color_done};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7735(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    ESP_ERROR_CHECK(gpio_set_level(PIN_BL, 1));

    s_fb = heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb) {
        ESP_LOGE(TAG, "no memory for framebuffer");
        abort();
    }
    memset(s_fb, 0, FRAME_PIXELS * sizeof(uint16_t));

    const gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN) |
                        (1ULL << PIN_BTN_OK) | (1ULL << PIN_BTN_BACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));
    ESP_LOGI(TAG, "ST7735 ready");
}

int st7735_button(void)
{
    static uint8_t stable;
    static uint32_t last;
    static uint32_t down_at[4];
    static bool was_down[4];
    static bool long_sent[4];

    uint8_t raw = 0;
    if (gpio_get_level(PIN_BTN_UP) == 0) raw |= 1;
    if (gpio_get_level(PIN_BTN_DOWN) == 0) raw |= 2;
    if (gpio_get_level(PIN_BTN_OK) == 0) raw |= 4;
    if (gpio_get_level(PIN_BTN_BACK) == 0) raw |= 8;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last < 20) return 0;
    last = now;
    stable = raw;

    for (int i = 0; i < 4; i++) {
        bool down = (stable >> i) & 1;
        if (down && !was_down[i]) {
            down_at[i] = now;
            long_sent[i] = false;
            was_down[i] = true;
            continue;
        }
        if (down && !long_sent[i] && now - down_at[i] >= BTN_LONG_MS) {
            long_sent[i] = true;
            return 11 + i;
        }
        if (!down && was_down[i]) {
            was_down[i] = false;
            if (!long_sent[i] && now - down_at[i] >= 30) return i + 1;
            long_sent[i] = false;
        }
    }
    return 0;
}
