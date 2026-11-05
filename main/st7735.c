/* ST7735 160x128 IoT finder UI.
 *
 * Layout is deliberately compact: large RSSI, a live bar, peer BSSID/SSID,
 * channel mode and the optional NRF RPD side channel.  Deep-green PHO theme
 * is the default requested for the handheld build.
 */
#include "st7735.h"

#include "nrf_aux.h"

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

/* Colour pipeline for this panel (verified with the on-device COLOR TEST page):
 *
 *   raw RGB565 + MADCTL BGR  ->  green/blue swap
 *                                (dark green looks blue, yellow looks magenta)
 *
 * So on this panel the BGR bit swaps G/B, not R/B.  Two swaps cancel: pack B
 * in the green field and G in the blue field here, and keep the panel on
 * LCD_RGB_ELEMENT_ORDER_BGR.  Do not change one without the other.
 *
 * Do not "fix" this by switching the panel to RGB: that removes only one of
 * the two swaps and leaves green/blue inverted. */
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((b) >> 2) << 5) | ((g) >> 3)))

/* Runtime palette.  The whole UI reads these variables, so the long-press
 * theme key can recolour every page without a reboot. */
static uint16_t col_bg, col_text, col_accent, col_dim, col_dark;
static uint16_t col_warn, col_orange, col_red;
static uint16_t col_rf_bg, col_rf_avg, col_rf_peak, col_rf_hot;

#define COL_BG     col_bg
#define COL_TEXT   col_text
#define COL_ACCENT col_accent
#define COL_DIM    col_dim
#define COL_DARK   col_dark
#define COL_WARN   col_warn
#define COL_ORANGE col_orange
#define COL_RED    col_red
#define COL_RF_BG   col_rf_bg
#define COL_RF_AVG  col_rf_avg
#define COL_RF_PEAK col_rf_peak
#define COL_RF_HOT  col_rf_hot

typedef struct {
    const char *name;
    uint8_t bg[3];
    uint8_t text[3];
    uint8_t accent[3];
    uint8_t dim[3];
    uint8_t dark[3];
    uint8_t warn[3];
    uint8_t orange[3];
    uint8_t red[3];
} ui_theme_t;

static const ui_theme_t s_themes[] = {
    /* 0 PHO: the original dark-green terminal */
    {"PHO",     {0x00,0x12,0x00}, {0xD8,0xFF,0xE4}, {0x38,0xFF,0x78},
                {0x18,0x7A,0x3C}, {0x00,0x24,0x00}, {0xFF,0xFF,0x00},
                {0xFF,0x80,0x00}, {0xFF,0x00,0x00}},
    /* 1 AMBER */
    {"AMBER",   {0x18,0x08,0x00}, {0xFF,0xE0,0xB0}, {0xFF,0xB0,0x20},
                {0xA0,0x60,0x10}, {0x30,0x12,0x00}, {0xFF,0xD0,0x20},
                {0xFF,0x80,0x10}, {0xFF,0x30,0x20}},
    /* 2 ICE */
    {"ICE",     {0x00,0x08,0x20}, {0xD0,0xF0,0xFF}, {0x40,0xC0,0xFF},
                {0x20,0x70,0xA0}, {0x00,0x20,0x40}, {0xFF,0xE0,0x60},
                {0xFF,0x80,0x20}, {0xFF,0x40,0x60}},
    /* 3 MAGENTA */
    {"MAGENTA", {0x18,0x00,0x18}, {0xFF,0xD0,0xF0}, {0xFF,0x40,0xC0},
                {0xA0,0x20,0x70}, {0x30,0x00,0x30}, {0xFF,0xE0,0x40},
                {0xFF,0x80,0x20}, {0xFF,0x30,0x40}},
    /* 4 RED ALERT */
    {"RED",     {0x20,0x00,0x00}, {0xFF,0xD0,0xC0}, {0xFF,0x50,0x40},
                {0xA0,0x20,0x20}, {0x40,0x00,0x00}, {0xFF,0xC0,0x20},
                {0xFF,0x70,0x10}, {0xFF,0x20,0x20}},
    /* 5 TEAL */
    {"TEAL",    {0x00,0x16,0x16}, {0xC0,0xFF,0xF0}, {0x30,0xE0,0xC0},
                {0x20,0x80,0x70}, {0x00,0x32,0x30}, {0xFF,0xD0,0x30},
                {0xFF,0x80,0x20}, {0xFF,0x40,0x40}},
    /* 6 BLUE/WHITE */
    {"BLUE",    {0x00,0x00,0x10}, {0xE0,0xE8,0xFF}, {0x80,0xC0,0xFF},
                {0x40,0x70,0xA0}, {0x10,0x10,0x30}, {0xFF,0xE0,0x60},
                {0xFF,0x80,0x20}, {0xFF,0x40,0x40}},
    /* 7 SOLAR */
    {"SOLAR",   {0x16,0x12,0x00}, {0xFF,0xF0,0xC0}, {0xFF,0xE0,0x40},
                {0xA0,0x80,0x20}, {0x32,0x24,0x00}, {0xFF,0xFF,0x60},
                {0xFF,0xA0,0x20}, {0xFF,0x50,0x20}},
    /* 8 VIOLET */
    {"VIOLET",  {0x0A,0x00,0x18}, {0xE0,0xD0,0xFF}, {0xA0,0x80,0xFF},
                {0x60,0x40,0xA0}, {0x20,0x00,0x30}, {0xFF,0xE0,0x40},
                {0xFF,0x80,0x30}, {0xFF,0x40,0x60}},
    /* 9 LIME */
    {"LIME",    {0x00,0x16,0x00}, {0xE0,0xFF,0xC0}, {0xA0,0xFF,0x20},
                {0x50,0x90,0x10}, {0x00,0x32,0x00}, {0xFF,0xE0,0x20},
                {0xFF,0x90,0x10}, {0xFF,0x30,0x20}},
};
#define THEME_COUNT ((int)(sizeof(s_themes) / sizeof(s_themes[0])))
static uint8_t s_theme_index;

static void apply_theme(uint8_t n)
{
    if (n >= THEME_COUNT) n = 0;
    s_theme_index = n;
    const ui_theme_t *t = &s_themes[n];
    col_bg     = RGB565(t->bg[0], t->bg[1], t->bg[2]);
    col_text   = RGB565(t->text[0], t->text[1], t->text[2]);
    col_accent = RGB565(t->accent[0], t->accent[1], t->accent[2]);
    col_dim    = RGB565(t->dim[0], t->dim[1], t->dim[2]);
    col_dark   = RGB565(t->dark[0], t->dark[1], t->dark[2]);
    col_warn   = RGB565(t->warn[0], t->warn[1], t->warn[2]);
    col_orange = RGB565(t->orange[0], t->orange[1], t->orange[2]);
    col_red    = RGB565(t->red[0], t->red[1], t->red[2]);
    col_rf_bg   = col_dark;
    col_rf_avg  = col_warn;
    col_rf_peak = col_red;
    col_rf_hot  = col_orange;
}

int st7735_next_theme(void)
{
    apply_theme((uint8_t)((s_theme_index + 1) % THEME_COUNT));
    return s_theme_index;
}

int st7735_theme(void)
{
    return s_theme_index;
}

const char *st7735_theme_name(void)
{
    return s_themes[s_theme_index].name;
}

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
    case '<': return 012421; case '>': return 042124; case '#': return 057575;
    case '*': return 052725;
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

static void draw_cjk12(int x, int y, const uint8_t *g, uint16_t color)
{
    for (int row = 0; row < 12; row++) {
        uint16_t bits = ((uint16_t)g[row * 2] << 8) | g[row * 2 + 1];
        for (int col = 0; col < 12; col++) {
            if (bits & (0x8000u >> col)) rect(x + col, y + row, 1, 1, color);
        }
    }
}

static ssid_enc_t ssid_chosen_enc(const finder_view_t *v, ssid_enc_t pref)
{
    size_t n = strlen(v->ssid);
    if (!v->ssid_nonascii) return SSID_ENC_ASCII;
    if (pref == SSID_ENC_UTF8 && ssid_utf8_valid((const uint8_t *)v->ssid, n))
        return SSID_ENC_UTF8;
    if (pref == SSID_ENC_GBK && ssid_gbk_valid((const uint8_t *)v->ssid, n))
        return SSID_ENC_GBK;
    return ssid_detect((const uint8_t *)v->ssid, n);
}

static void draw_ssid_text(const char *text, ssid_enc_t enc, uint16_t color)
{
    size_t n = strlen(text);
    size_t pos = 0;
    int x = 4, y = 34, line = 0;

    while (pos < n && line < 5) {
        uint32_t cp;
        size_t used = ssid_decode((const uint8_t *)text, n, pos, enc, &cp);
        if (!used) break;
        pos += used;

        int adv = 8;
        const uint8_t *glyph = NULL;
        if (cp >= 0x80) {
            glyph = cjk_glyph(cp);
            adv = glyph ? 12 : 8;
        }
        if (x + adv > 156) {
            x = 4;
            y += 15;
            line++;
            if (line >= 5) break;
        }

        if (cp >= 0x80 && glyph) {
            draw_cjk12(x, y, glyph, color);
        } else {
            char one[2] = {(char)((cp >= 0x80) ? '?' : cp), 0};
            text8(x, y + 2, one, 1, color);
        }
        x += adv;
    }
}

static void draw_ssid_page(const finder_view_t *v, const ui_state_t *ui)
{
    char buf[80], mac[18];
    size_t n = strlen(v->ssid);
    ssid_enc_t enc = ssid_chosen_enc(v, ui->ssid_enc);
    bool u8 = v->ssid_nonascii &&
              ssid_utf8_valid((const uint8_t *)v->ssid, n);
    bool gbk = v->ssid_nonascii &&
               ssid_gbk_valid((const uint8_t *)v->ssid, n);

    clear_panel();

    text3x5(2, 2, "SSID", 2, COL_ACCENT);
    text3x5(104, 3, ssid_enc_name(enc), 1, COL_DIM);
    text3x5(126, 3, "* BACK", 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (v->have_bssid) {
        mac_text(v->bssid, mac);
        snprintf(buf, sizeof(buf), "BSSID %s", mac);
    } else {
        snprintf(buf, sizeof(buf), "BSSID --");
    }
    text3x5(2, 16, buf, 1, COL_DIM);

    if (v->have_ap_load) {
        unsigned pct = (v->ap_load * 100u + 127u) / 255u;
        snprintf(buf, sizeof(buf), "CH %u  AP %u%%/%u", v->channel, pct,
                 v->ap_sta_count);
    } else {
        snprintf(buf, sizeof(buf), "CH %u  AP --", v->channel);
    }
    text3x5(2, 23, buf, 1, COL_TEXT);

    if (!v->have_ssid || !n) {
        if (v->ssid_hidden) {
            text3x5(4, 40, "SSID HIDDEN", 2, COL_WARN);
            text3x5(4, 58, "AP ADVERTISES NO SSID", 1, COL_DIM);
        } else if (v->bss_known) {
            text3x5(4, 40, "SSID ?", 2, COL_DIM);
            text3x5(4, 58, "AP HAS NO SSID", 1, COL_DIM);
        } else {
            text3x5(4, 40, "NO SSID", 2, COL_DIM);
            text3x5(4, 58, v->have_bssid ? "AP BEACON NOT HEARD"
                                         : "NO ASSOCIATED AP", 1, COL_DIM);
        }
    } else {
        draw_ssid_text(v->ssid, enc, COL_TEXT);
    }

    if (u8 && gbk) {
        text3x5(2, 119, "# ENC", 1, COL_DIM);
        text3x5(40, 119, "* BACK", 1, COL_ACCENT);
    } else {
        text3x5(2, 119, "* BACK", 1, COL_ACCENT);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

/* Single-line variant of draw_ssid_text, used by the AP/STA tree. */
static int draw_ssid_line(int x, int y, const char *text, ssid_enc_t enc,
                          int max_x, uint16_t color)
{
    size_t n = strlen(text);
    size_t pos = 0;

    while (pos < n) {
        uint32_t cp;
        size_t used = ssid_decode((const uint8_t *)text, n, pos, enc, &cp);
        if (!used) break;
        pos += used;

        int adv = 8;
        const uint8_t *glyph = NULL;
        if (cp >= 0x80) {
            glyph = cjk_glyph(cp);
            adv = glyph ? 12 : 8;
        }
        if (x + adv > max_x) break;

        if (cp >= 0x80 && glyph) {
            draw_cjk12(x, y, glyph, color);
        } else {
            char one[2] = {(char)((cp >= 0x80) ? '?' : cp), 0};
            text8(x, y + 2, one, 1, color);
        }
        x += adv;
    }
    return x;
}

static void cjk_line(int x, int y, const char *s, uint16_t color)
{
    draw_ssid_line(x, y, s, SSID_ENC_UTF8, 156, color);
}

static ssid_enc_t ap_ssid_enc(const finder_ap_t *a, ssid_enc_t pref)
{
    if (!a->ssid_len || a->binary) return SSID_ENC_ASCII;
    size_t n = strlen(a->ssid);
    if (!n) return SSID_ENC_ASCII;
    if (pref == SSID_ENC_UTF8 && ssid_utf8_valid((const uint8_t *)a->ssid, n))
        return SSID_ENC_UTF8;
    if (pref == SSID_ENC_GBK && ssid_gbk_valid((const uint8_t *)a->ssid, n))
        return SSID_ENC_GBK;
    return ssid_detect((const uint8_t *)a->ssid, n);
}

static void ap_name(const finder_ap_t *a, char *out, size_t n)
{
    if (a->hidden || a->binary) {
        snprintf(out, n, "<%02X%02X%02X>",
                 a->bssid[3], a->bssid[4], a->bssid[5]);
    } else {
        snprintf(out, n, "%s", a->ssid);
    }
}

#define AP_ROWS 8
#define STA_ROWS 7
#define AP_ROW_Y 16
#define AP_ROW_H 13
#define STA_ROW_Y 34
#define STA_ROW_H 12

static void draw_ap_list_page(const ui_state_t *ui)
{
    char buf[48], name[40];

    clear_panel();
    text3x5(2, 2, "AP LIST", 2, COL_ACCENT);
    const char *etag = ui->ssid_enc == SSID_ENC_UTF8 ? "U8" :
                       ui->ssid_enc == SSID_ENC_GBK ? "GBK" : "AUTO";
    text3x5(96, 3, etag, 1, COL_DIM);
    snprintf(buf, sizeof(buf), "%d AP", ui->ap_count);
    text3x5(126, 3, buf, 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (ui->ap_count == 0) {
        text3x5(4, 38, "WAIT FOR AP BEACONS", 1, COL_DIM);
        text3x5(4, 46, "KEEP CRUISING 1-13", 1, COL_DIM);
    } else {
        for (int row = 0; row < AP_ROWS; row++) {
            int idx = ui->ap_scroll + row;
            if (idx >= ui->ap_count) break;
            const finder_ap_t *a = &ui->aps[idx];
            int y = AP_ROW_Y + row * AP_ROW_H;
            bool sel = idx == ui->ap_sel;
            if (sel) rect(0, y - 1, WIDTH, AP_ROW_H, COL_DARK);

            snprintf(buf, sizeof(buf), "%02u", a->channel);
            text3x5(2, y + 4, buf, 1, sel ? COL_ACCENT : COL_DIM);

            ap_name(a, name, sizeof(name));
            if (a->hidden || a->binary) {
                text3x5(24, y + 4, name, 1, sel ? COL_ACCENT : COL_DIM);
            } else {
                draw_ssid_line(24, y, a->ssid, ap_ssid_enc(a, ui->ssid_enc), 128,
                               sel ? COL_ACCENT : COL_TEXT);
            }

            if (a->has_load) {
                snprintf(buf, sizeof(buf), "%u/%u", a->seen_sta, a->load_sta);
            } else {
                snprintf(buf, sizeof(buf), "%u", a->seen_sta);
            }
            text3x5(134, y + 4, buf, 1, sel ? COL_ACCENT : COL_DIM);
        }
    }

    text3x5(2, 119, "> < MOVE  # INFO  * BACK", 1, COL_DIM);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_ap_detail_page(const ui_state_t *ui)
{
    char buf[64], tmp[48], mac[18], age[16], name[40];
    const finder_ap_t *a = NULL;
    if (ui->ap_sel >= 0 && ui->ap_sel < ui->ap_count) a = &ui->aps[ui->ap_sel];

    clear_panel();
    text3x5(2, 2, "AP DETAIL", 2, COL_ACCENT);
    const char *etag = ui->ssid_enc == SSID_ENC_UTF8 ? "U8" :
                       ui->ssid_enc == SSID_ENC_GBK ? "GBK" : "AUTO";
    text3x5(100, 3, etag, 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (!a) {
        text3x5(4, 40, "NO AP SELECTED", 1, COL_DIM);
        text3x5(2, 119, "* BACK", 1, COL_ACCENT);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
        if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
        return;
    }

    if (a->hidden || a->binary) {
        ap_name(a, name, sizeof(name));
        text3x5(2, 14, name, 2, COL_WARN);
        text3x5(100, 19, a->hidden ? "HIDDEN" : "BINARY", 1, COL_WARN);
    } else {
        draw_ssid_line(2, 14, a->ssid, ap_ssid_enc(a, ui->ssid_enc), 156, COL_TEXT);
    }

    mac_text(a->bssid, mac);
    snprintf(buf, sizeof(buf), "BSSID %s", mac);
    text3x5(2, 29, buf, 1, COL_TEXT);

    fit_text(tmp, sizeof(tmp), a->vendor);
    tmp[31] = 0;
    text3x5(2, 36, "VENDOR", 1, COL_DIM);
    text3x5(38, 36, tmp, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "CH %u   SEEN %u STA", a->channel, a->seen_sta);
    text3x5(2, 43, buf, 1, COL_TEXT);

    if (a->has_load) {
        unsigned pct = (a->load_util * 100u + 127u) / 255u;
        snprintf(buf, sizeof(buf), "AP %u STA   %u%% BUSY", a->load_sta, pct);
    } else {
        snprintf(buf, sizeof(buf), "AP LOAD --");
    }
    text3x5(2, 50, buf, 1, COL_TEXT);

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    age_text(age, sizeof(age), now - a->last_ms, true);
    if (a->rssi < -120) {
        snprintf(buf, sizeof(buf), "RSSI --   LAST %s", age);
    } else {
        snprintf(buf, sizeof(buf), "RSSI %d dBm   LAST %s", a->rssi, age);
    }
    text3x5(2, 57, buf, 1, COL_TEXT);

    if (a->seen_sta) {
        snprintf(buf, sizeof(buf), "%u STA(s) currently seen", a->seen_sta);
        text3x5(2, 71, buf, 1, COL_DIM);
    } else {
        text3x5(2, 71, "NO STA FRAMES SEEN YET", 1, COL_DIM);
        text3x5(2, 78, "AP REPORT COUNT IS IN BSS LOAD", 1, COL_DIM);
    }

    text3x5(2, 119, "# STA LIST   * BACK", 1, COL_ACCENT);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_sta_list_page(const ui_state_t *ui)
{
    char buf[64], mac[18], age[16], name[40], ven[16];
    const finder_ap_t *a = NULL;
    if (ui->ap_sel >= 0 && ui->ap_sel < ui->ap_count) a = &ui->aps[ui->ap_sel];

    clear_panel();
    text3x5(2, 2, "STA LIST", 2, COL_ACCENT);
    const char *etag = ui->ssid_enc == SSID_ENC_UTF8 ? "U8" :
                       ui->ssid_enc == SSID_ENC_GBK ? "GBK" : "AUTO";
    text3x5(96, 3, etag, 1, COL_DIM);
    snprintf(buf, sizeof(buf), "%d STA", ui->sta_count);
    text3x5(126, 3, buf, 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (a) {
        if (a->hidden || a->binary) {
            ap_name(a, name, sizeof(name));
            text3x5(2, 14, name, 1, COL_WARN);
        } else {
            draw_ssid_line(2, 13, a->ssid, ap_ssid_enc(a, ui->ssid_enc), 132, COL_DIM);
        }
        mac_text(a->bssid, mac);
        snprintf(buf, sizeof(buf), "AP %s", mac);
        text3x5(2, 27, buf, 1, COL_DIM);
    }

    if (!ui->sta_count) {
        text3x5(4, 42, "NO STA SEEN ON THIS AP", 1, COL_DIM);
        text3x5(4, 50, "ACTIVE 60S / ALL CHANNELS", 1, COL_DIM);
    } else {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        for (int row = 0; row < STA_ROWS; row++) {
            int idx = ui->sta_scroll + row;
            if (idx >= ui->sta_count) break;
            const finder_sta_t *st = &ui->stas[idx];
            int y = STA_ROW_Y + row * STA_ROW_H;
            bool sel = idx == ui->sta_sel;
            if (sel) rect(0, y - 1, WIDTH, STA_ROW_H, COL_DARK);

            mac_text(st->mac, mac);
            text3x5(2, y + 2, mac, 1, sel ? COL_ACCENT : COL_TEXT);

            snprintf(buf, sizeof(buf), "%d", st->rssi);
            text3x5(74, y + 2, buf, 1, sel ? COL_ACCENT : COL_DIM);

            if (st->probe_ssid_len) {
                snprintf(ven, sizeof(ven), "%.8s", st->probe_ssid);
            } else {
                snprintf(ven, sizeof(ven), "%.8s", st->vendor);
            }
            text3x5(92, y + 2, ven, 1, sel ? COL_ACCENT : COL_DIM);

            age_text(age, sizeof(age), now - st->last_ms, true);
            text3x5(136, y + 2, age, 1, sel ? COL_ACCENT : COL_DIM);
        }
    }

    text3x5(2, 119, "# TARGET   * BACK", 1, COL_ACCENT);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void menu_label(int item, const finder_view_t *v, char *out, size_t n)
{
    switch (item) {
    case UI_MENU_LEARN: snprintf(out, n, "LEARN TARGET"); break;
    case UI_MENU_CLEAR: snprintf(out, n, "CLEAR TARGET"); break;
    case UI_MENU_PEAK: snprintf(out, n, "PEAK CLEAR"); break;
    case UI_MENU_CHAN:
        snprintf(out, n, "CHAN %s", mode_name(v->chan_mode));
        break;
    case UI_MENU_SSID: snprintf(out, n, "AP LIST"); break;
    case UI_MENU_RFSCAN: snprintf(out, n, "RF SCAN"); break;
    case UI_MENU_INFO: snprintf(out, n, "INFO"); break;
    case UI_MENU_REBOOT: snprintf(out, n, "REBOOT"); break;
    default: snprintf(out, n, "--"); break;
    }
}

static void draw_menu_page(const finder_view_t *v, const ui_state_t *ui)
{
    char buf[40];
    int sel = ui->menu_sel;
    if (sel < 0 || sel >= UI_MENU_COUNT) sel = 0;

    clear_panel();
    text3x5(2, 2, "MENU", 2, COL_ACCENT);
    text3x5(126, 3, "* BACK", 1, COL_DIM);
    rect(0, 12, WIDTH, 1, COL_DARK);

    for (int i = 0; i < UI_MENU_COUNT; i++) {
        int y = 20 + i * 12;
        if (i == sel) {
            rect(0, y - 2, WIDTH, 13, COL_DARK);
            text3x5(4, y, ">", 2, COL_ACCENT);
        }
        menu_label(i, v, buf, sizeof(buf));
        text3x5(18, y, buf, 2, i == sel ? COL_TEXT : COL_DIM);
    }

    text3x5(2, 119, "> < MOVE  # SELECT  * BACK", 1, COL_DIM);

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_rf_scan_page(const ui_state_t *ui)
{
    (void)ui;
    nrf_scan_ch_t sc[NRF_SCAN_CHANNELS];
    nrf_aux_scan_get(sc, NRF_SCAN_CHANNELS);

    char buf[64];
    clear_panel();
    text3x5(2, 2, "RF SCAN", 2, COL_ACCENT);
    snprintf(buf, sizeof(buf), "S%lu",
             (unsigned long)nrf_aux_scan_cycles());
    text3x5(64, 3, buf, 1, COL_DIM);
    if (nrf_aux_ok()) {
        if (nrf_aux_ok2() && nrf_aux_csn2() >= 0)
            snprintf(buf, sizeof(buf), "C%d+C%d", nrf_aux_csn(), nrf_aux_csn2());
        else
            snprintf(buf, sizeof(buf), "C%d", nrf_aux_csn());
        text3x5(108, 3, buf, 1, COL_ACCENT);
    } else {
        text3x5(118, 3, "NRF --", 1, COL_DIM);
    }
    rect(0, 12, WIDTH, 1, COL_DARK);

    if (!nrf_aux_ok()) {
        text3x5(4, 38, "NRF NOT FOUND", 1, COL_WARN);
        text3x5(4, 46, "CHECK 3V3 / SPI / CSN", 1, COL_DIM);
        text3x5(2, 119, "* BACK", 1, COL_ACCENT);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
        if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
        return;
    }

    const int x0 = 8, bw = 8, gap = 3, top = 22, bottom = 100;
    for (int ch = 0; ch < NRF_SCAN_CHANNELS; ch++) {
        int x = x0 + ch * (bw + gap);
        int peak = sc[ch].peak;
        int avg = sc[ch].avg;
        int h = bottom - top;

        rect(x, top, bw, h, COL_RF_BG);
        if (peak) {
            int hp = h * peak / 100;
            if (hp < 1) hp = 1;
            uint16_t col = peak >= 80 ? COL_RF_HOT : COL_RF_PEAK;
            rect(x, bottom - hp, bw, hp, col);
        }
        if (avg) {
            int ha = h * avg / 100;
            if (ha < 1) ha = 1;
            rect(x + 2, bottom - ha, bw - 4, ha, COL_RF_AVG);
        }

        char num[4];
        snprintf(num, sizeof(num), "%d", ch + 1);
        text3x5(x + (ch < 9 ? 2 : 0), 104, num, 1, COL_DIM);
    }

    /* Keep the displayed channel stable; otherwise a slightly higher peak
     * on another channel makes HIT x/y look like it went backwards. */
    static int shown = -1;
    static uint8_t shown_peak;
    static uint32_t shown_cycles;
    uint32_t cycles = nrf_aux_scan_cycles();
    if (cycles < shown_cycles) {
        shown = -1;
        shown_peak = 0;
    }
    shown_cycles = cycles;

    int maxch = -1;
    uint8_t maxpeak = 0;
    for (int ch = 0; ch < NRF_SCAN_CHANNELS; ch++) {
        if (sc[ch].peak > maxpeak) {
            maxpeak = sc[ch].peak;
            maxch = ch;
        }
    }
    if (maxch >= 0) {
        if (shown < 0 || maxpeak > (int)shown_peak + 10) {
            shown = maxch;
            shown_peak = maxpeak;
        } else {
            shown_peak = sc[shown].peak;
        }
    }

    if (shown >= 0 && sc[shown].peak > 0) {
        int best = shown;
        uint8_t best_peak = sc[shown].peak;
        uint32_t age_s = sc[best].peak_age_ms / 1000;
        if (age_s > 99) age_s = 99;
        /* Keep the original HIT x/y in the summary line. */
        snprintf(buf, sizeof(buf), "CH%u PK%u AGE%lus HIT%u/%u",
                 best + 1, best_peak, (unsigned long)age_s,
                 sc[best].hits, sc[best].sweeps);
        text3x5(2, 112, buf, 1, COL_WARN);

        /* New role: the x of HIT x/y (hit count), 2x and right-aligned. */
        char hit[8];
        snprintf(hit, sizeof(hit), "%u", sc[best].hits);
        int hw = (int)strlen(hit) * 8;
        int hx = 158 - hw;
        if (hx < 112) hx = 112;
        text3x5(hx, 109, hit, 2, COL_WARN);

        /* Hit rate below it: hits per second since the last scan reset. */
        char rate[16];
        uint32_t elapsed = nrf_aux_scan_elapsed_ms();
        if (elapsed >= 1000 && sc[best].hits) {
            uint32_t tenths = (uint32_t)sc[best].hits * 10000u / elapsed;
            if (tenths >= 1000)
                snprintf(rate, sizeof(rate), "%lu/s",
                         (unsigned long)(tenths / 10));
            else
                snprintf(rate, sizeof(rate), "%lu.%lu/s",
                         (unsigned long)(tenths / 10),
                         (unsigned long)(tenths % 10));
        } else {
            snprintf(rate, sizeof(rate), "--/s");
        }
        int rw = (int)strlen(rate) * 4;
        int rx = 158 - rw;
        if (rx < 112) rx = 112;
        text3x5(rx, 119, rate, 1, COL_DIM);
    } else {
        text3x5(2, 112, "NO ENERGY YET", 1, COL_DIM);
    }
    text3x5(2, 119, "# HELP  HOLD# RESET  * BACK", 1, COL_ACCENT);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_rf_help_page(void)
{
    clear_panel();
    cjk_line(2, 2, "RF 图表说明", COL_ACCENT);
    rect(0, 15, WIDTH, 1, COL_DARK);

    cjk_line(2, 18, "1. 13柱=信道1-13", COL_TEXT);
    cjk_line(2, 31, "2. 外柱=峰值保持", COL_TEXT);
    cjk_line(2, 44, "3. 内柱=平均值", COL_TEXT);
    cjk_line(2, 57, "4. 空柱=无能量", COL_TEXT);
    cjk_line(2, 70, "5. PK峰值 AGE多久前", COL_TEXT);
    cjk_line(2, 83, "6. HIT>=25%轮数", COL_TEXT);
    cjk_line(2, 96, "7. 红<80 橙>=80", COL_TEXT);

    text3x5(2, 119, "* BACK", 1, COL_ACCENT);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_color_test_page(void)
{
    static const uint16_t bars[8] = {
        RGB565(0xFF, 0x00, 0x00), /* R */
        RGB565(0x00, 0xFF, 0x00), /* G */
        RGB565(0x00, 0x00, 0xFF), /* B */
        RGB565(0xFF, 0xFF, 0x00), /* Y */
        RGB565(0x00, 0xFF, 0xFF), /* C */
        RGB565(0xFF, 0x00, 0xFF), /* M */
        RGB565(0xFF, 0xFF, 0xFF), /* W */
        RGB565(0x00, 0x00, 0x00), /* K */
    };
    static const char *names[8] = {"R","G","B","Y","C","M","W","K"};

    clear_panel();
    text3x5(2, 2, "COLOR TEST", 2, COL_TEXT);
    rect(0, 12, WIDTH, 1, COL_DARK);
    for (int i = 0; i < 8; i++) {
        int x = 4 + i * 19;
        rect(x, 24, 16, 78, bars[i]);
        text3x5(x + 6, 106, names[i], 1, COL_TEXT);
    }
    text3x5(2, 119, "* BACK", 1, COL_ACCENT);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_finder_page(const finder_view_t *v, const wnm_clock_view_t *c)
{
    char buf[96], tmp[64];
    bool have_rssi = v->have_rssi;

    clear_panel();

    char clk[16], date[16];
    uint16_t clock_col = COL_DIM;
    if (c && c->synced) {
        snprintf(date, sizeof(date), "%04u-%02u-%02u",
                 c->year, c->month, c->day);
        snprintf(clk, sizeof(clk), "%02u:%02u:%02u", c->hour, c->min, c->sec);
        clock_col = COL_ACCENT;
        text3x5(4, 1, date, 2, clock_col);
        text3x5(90, 1, clk, 2, clock_col);
    } else {
        service_clock(clk, sizeof(clk), true);
        text3x5(70, 1, "UP", 2, clock_col);
        text3x5(90, 1, clk, 2, clock_col);
    }
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

    text3x5(4, 72, "PEAK", 1, COL_TEXT);
    if (v->peak < -120) {
        text2x5(24, 72, "--", 1, COL_TEXT);
    } else {
        snprintf(tmp, sizeof(tmp), "%d", v->peak);
        text2x5(24, 72, tmp, 1, COL_TEXT);
    }
    text3x5(120, 72, "FPS", 1, COL_TEXT);
    snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v->fps);
    text2x5(136, 72, tmp, 1, COL_TEXT);

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
        if (v->ssid_nonascii) {
            ssid_enc_t e = ssid_detect((const uint8_t *)v->ssid, strlen(v->ssid));
            snprintf(buf, sizeof(buf), "SSID [%s]  * MENU", ssid_enc_name(e));
        } else {
            fit_text(tmp, sizeof(tmp), v->ssid);
            snprintf(buf, sizeof(buf), "SSID %s", tmp);
        }
    } else if (v->ssid_hidden) {
        snprintf(buf, sizeof(buf), "SSID <HIDDEN>  * MENU");
    } else if (v->bss_known) {
        snprintf(buf, sizeof(buf), "SSID ?  * MENU");
    } else {
        snprintf(buf, sizeof(buf), "SSID --");
    }
    text3x5(4, 94, buf, 1, COL_TEXT);

    snprintf(buf, sizeof(buf), "CH %u %s", v->channel, mode_name(v->chan_mode));
    text3x5(4, 101, buf, 1, COL_TEXT);
    text3x5(72, 101, "CAND", 1, COL_DIM);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)v->cand_count);
    text2x5(96, 101, buf, 1, COL_DIM);
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
        text3x5(2, 114, "> NEXT < PREV  # LEARN  * MENU", 1, COL_DIM);
    } else {
        text3x5(2, 114, "> NEXT < PREV  # LOCK/HOP  * MENU", 1, COL_DIM);
    }

    if (v->learning) {
        text3x5(2, 121, "HOLD STILL", 1, COL_DIM);
    } else {
        text3x5(2, 121, "HOLD >/< CH   HOLD * THEME", 1, COL_DIM);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

static void draw_info_page(const finder_view_t *v, const wnm_clock_view_t *c)
{
    char buf[96], tmp[64], mac[18], pk[8];

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
        if (v->ssid_nonascii) {
            ssid_enc_t e = ssid_detect((const uint8_t *)v->ssid, strlen(v->ssid));
            snprintf(buf, sizeof(buf), "SSID [%s]  * MENU > AP LIST", ssid_enc_name(e));
        } else {
            fit_text(tmp, sizeof(tmp), v->ssid);
            snprintf(buf, sizeof(buf), "SSID %s", tmp);
        }
    } else if (v->ssid_hidden) {
        snprintf(buf, sizeof(buf), "SSID <HIDDEN>  * MENU > SSID");
    } else if (v->bss_known) {
        snprintf(buf, sizeof(buf), "SSID ?  * MENU > SSID");
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

    if (v->nrf_ok && v->nrf_csn >= 0) {
        char csn[16];
        if (v->nrf_ok2 && v->nrf_csn2 >= 0)
            snprintf(csn, sizeof(csn), "C%d+C%d", v->nrf_csn, v->nrf_csn2);
        else
            snprintf(csn, sizeof(csn), "C%d", v->nrf_csn);
        if (v->have_ap_load) {
            unsigned ap_pct = (v->ap_load * 100u + 127u) / 255u;
            snprintf(buf, sizeof(buf), "LINK %s RF %s %u%% AP %u%%/%u",
                     link_name(v->link), csn, v->nrf_duty,
                     ap_pct, v->ap_sta_count);
        } else {
            snprintf(buf, sizeof(buf), "LINK %s RF %s %u%%",
                     link_name(v->link), csn, v->nrf_duty);
        }
    } else if (v->have_ap_load) {
        unsigned ap_pct = (v->ap_load * 100u + 127u) / 255u;
        snprintf(buf, sizeof(buf), "LINK %s RF -- AP %u%%/%u",
                 link_name(v->link), ap_pct, v->ap_sta_count);
    } else {
        snprintf(buf, sizeof(buf), "LINK %s RF --", link_name(v->link));
    }
    text3x5(2, 66, buf, 1, COL_TEXT);

    if (c && c->synced) {
        snprintf(buf, sizeof(buf), "TIME %04u-%02u-%02u %02u:%02u:%02u",
                 c->year, c->month, c->day, c->hour, c->min, c->sec);
        text3x5(2, 73, buf, 1, COL_TEXT);
    } else {
        text3x5(2, 73, "TIME WAITING FOR AP", 1, COL_DIM);
    }

    text3x5(2, 82, "> NEXT CAND   < PREV CAND", 1, COL_DIM);
    text3x5(2, 89, "HOLD >/<  CHANNEL", 1, COL_DIM);
    text3x5(2, 96, "# LOCK/HOP  HOLD # LEARN", 1, COL_DIM);
    text3x5(2, 103, "* MENU/BACK  HOLD * THEME", 1, COL_DIM);
    text3x5(2, 110, "AP LIST: * MENU > AP LIST", 1, COL_DIM);

    char src[16] = "--";
    if (c && c->synced && c->have_source) {
        snprintf(src, sizeof(src), "%02X:%02X:%02X", c->source[3], c->source[4], c->source[5]);
    }
    snprintf(buf, sizeof(buf), "SYNC %s", src);
    text3x5(2, 119, buf, 1, (c && c->synced) ? COL_ACCENT : COL_DIM);
    text3x5(60, 119, "* BACK", 1, COL_ACCENT);

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, WIDTH, HEIGHT, s_fb));
    if (s_flush_done) xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
}

void st7735_draw_finder(const finder_view_t *v, const wnm_clock_view_t *c,
                        const ui_state_t *ui)
{
    if (!s_panel || !s_fb || !v || !ui) return;
    switch (ui->page) {
    case UI_PAGE_MENU: draw_menu_page(v, ui); break;
    case UI_PAGE_INFO: draw_info_page(v, c); break;
    case UI_PAGE_SSID: draw_ssid_page(v, ui); break;
    case UI_PAGE_AP_LIST: draw_ap_list_page(ui); break;
    case UI_PAGE_AP_DETAIL: draw_ap_detail_page(ui); break;
    case UI_PAGE_AP_STAS: draw_sta_list_page(ui); break;
    case UI_PAGE_RF_SCAN: draw_rf_scan_page(ui); break;
    case UI_PAGE_RF_HELP: draw_rf_help_page(); break;
    case UI_PAGE_COLOR_TEST: draw_color_test_page(); break;
    case UI_PAGE_FIND:
    default: draw_finder_page(v, c); break;
    }
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
    apply_theme(0);
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
    static uint8_t last_raw;
    static uint8_t stable_cnt;
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
    if (raw == last_raw) {
        if (stable_cnt < 4) stable_cnt++;
    } else {
        last_raw = raw;
        stable_cnt = 0;
    }
    if (stable_cnt < 2) return 0;
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
