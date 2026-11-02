#pragma once

#include "finder.h"
#include "wnm_clock.h"

void st7735_init(void);
void st7735_draw_finder(const finder_view_t *v, const wnm_clock_view_t *c, int page);
int st7735_button(void);
