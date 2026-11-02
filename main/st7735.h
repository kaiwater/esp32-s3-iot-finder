#pragma once

#include "finder.h"
#include "wnm_clock.h"
#include "cjk_font.h"

void st7735_init(void);
typedef enum {
    UI_PAGE_FIND = 0,
    UI_PAGE_MENU,
    UI_PAGE_INFO,
    UI_PAGE_SSID,
} ui_page_t;

enum {
    UI_MENU_LEARN = 0,
    UI_MENU_CLEAR,
    UI_MENU_PEAK,
    UI_MENU_CHAN,
    UI_MENU_SSID,
    UI_MENU_INFO,
    UI_MENU_REBOOT,
    UI_MENU_COUNT,
};

typedef struct {
    ui_page_t page;
    int menu_sel;
    ssid_enc_t ssid_enc;
} ui_state_t;

void st7735_draw_finder(const finder_view_t *v, const wnm_clock_view_t *c,
                        const ui_state_t *ui);
int st7735_button(void);
