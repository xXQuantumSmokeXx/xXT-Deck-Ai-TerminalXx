#pragma once
#include <stdint.h>

struct ThemeEntry {
    const char *name;
    uint16_t    color;
};

extern const ThemeEntry THEME_COLORS[];
extern const int        THEME_COLOR_COUNT;
extern uint16_t         g_themeColor;

void        themeColorInit();
void        themeColorSet(int idx);
int         themeColorIndex();
const char *themeColorName();
