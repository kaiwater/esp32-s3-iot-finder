#pragma once

#include "finder.h"
#include "wnm_clock.h"
#include "cjk_font.h"

#define UI_AP_ROWS 8
#define UI_STA_ROWS 7

void st7735_init(void);
typedef enum {
    UI_PAGE_FIND = 0,
    UI_PAGE_MENU,
    UI_PAGE_INFO,
    UI_PAGE_SSID,
    UI_PAGE_AP_LIST,
    UI_PAGE_AP_DETAIL,
    UI_PAGE_AP_STAS,
    UI_PAGE_RF_SCAN,
} ui_page_t;

enum {
    UI_MENU_LEARN = 0,
    UI_MENU_CLEAR,
    UI_MENU_PEAK,
    UI_MENU_CHAN,
    UI_MENU_SSID,
    UI_MENU_RFSCAN,
    UI_MENU_INFO,
    UI_MENU_REBOOT,
    UI_MENU_COUNT,
};

typedef struct {
    ui_page_t page;
    int menu_sel;
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
