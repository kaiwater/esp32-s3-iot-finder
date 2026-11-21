#pragma once

#include "finder.h"
#include "wnm_clock.h"
#include "cjk_font.h"

#define UI_AP_ROWS 8
#define UI_STA_ROWS 7

void st7735_init(void);
int st7735_next_theme(void);
int st7735_theme(void);
int st7735_rf_shown_channel(void);

/* Bring-up helpers: re-init the panel and hold a solid colour on screen. */
void st7735_lcd_reinit(void);
void st7735_lcd_test(int which);
void st7735_lcd_test_off(void);
void st7735_lcd_status(void);
void st7735_lcd_recover(void);
void st7735_lcd_boot_init(int attempts);
void st7735_set_auto_reinit(uint16_t sec);
uint16_t st7735_get_auto_reinit(void);

/* RF Scan Pro overlay colours and water scale (persisted in NVS). */
typedef enum {
    ST7735_PRO_FRAME = 0,
    ST7735_PRO_NUM,
    ST7735_PRO_WATER,
    ST7735_PRO_MGMT,
} st7735_pro_color_t;

void st7735_pro_defaults(void);
void st7735_pro_load(void);
void st7735_pro_save(void);
void st7735_set_pro_color(st7735_pro_color_t which, uint16_t color);
uint16_t st7735_get_pro_color(st7735_pro_color_t which);
void st7735_set_pro_full(uint16_t fps);
uint16_t st7735_get_pro_full(void);
uint16_t st7735_rgb565(uint8_t r, uint8_t g, uint8_t b);
void st7735_rgb888(uint16_t color, uint8_t *r, uint8_t *g, uint8_t *b);
const char *st7735_theme_name(void);
typedef enum {
    UI_PAGE_FIND = 0,
    UI_PAGE_MENU,
    UI_PAGE_INFO,
    UI_PAGE_SSID,
    UI_PAGE_AP_LIST,
    UI_PAGE_AP_DETAIL,
    UI_PAGE_AP_STAS,
    UI_PAGE_RF_SCAN,
    UI_PAGE_RF_HELP,
    UI_PAGE_MW,
    UI_PAGE_RF_PRO,
    UI_PAGE_COLOR_TEST,
} ui_page_t;

enum {
    UI_MENU_LEARN = 0,
    UI_MENU_CLEAR,
    UI_MENU_PEAK,
    UI_MENU_CHAN,
    UI_MENU_SSID,
    UI_MENU_RFSCAN,
    UI_MENU_MW,
    UI_MENU_RFPRO,
    UI_MENU_INFO,
    UI_MENU_REBOOT,
    UI_MENU_COUNT,
};

typedef struct {
    ui_page_t page;
    ui_page_t help_back;
    int menu_sel;
    int pro_view;
    ssid_enc_t ssid_enc;

    int ap_sel;
    int ap_scroll;
    int ap_count;
    int sta_sel;
    int sta_scroll;
    int sta_count;
    finder_ap_t aps[FINDER_AP_MAX];
    finder_sta_t stas[FINDER_STA_MAX];
} ui_state_t;

void st7735_draw_finder(const finder_view_t *v, const wnm_clock_view_t *c,
                        const ui_state_t *ui);
int st7735_button(void);
