#pragma once

#include "finder.h"

void st7735_init(void);
void st7735_draw_finder(const finder_view_t *v, int page);
int st7735_button(void);
