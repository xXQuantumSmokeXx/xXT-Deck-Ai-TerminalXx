#include "theme_color.h"
#include "../config/nvs_config.h"

const ThemeEntry THEME_COLORS[] = {
    { "CYAN",   0x07FF },  // #00FFFF
    { "GREEN",  0x07E0 },  // #00FF00
    { "RED",    0xF800 },  // #FF0000
    { "ORANGE", 0xFC60 },  // #FF8D00
    { "YELLOW", 0xFFE0 },  // #FFFF00
    { "GRAY",   0x8410 },  // #848284
    { "PURPLE", 0xA81F },  // #AB00FF
    { "PINK",   0xFB56 },  // #FF69B0
    { "WHITE",  0xFFFF },  // #FFFFFF
};
const int THEME_COLOR_COUNT = (int)(sizeof(THEME_COLORS) / sizeof(THEME_COLORS[0]));

uint16_t g_themeColor = 0x07FF;

void themeColorInit() {
    int idx = nvsGetInt("theme_idx", 0);
    if (idx < 0 || idx >= THEME_COLOR_COUNT) idx = 0;
    g_themeColor = THEME_COLORS[idx].color;
}

void themeColorSet(int idx) {
    if (idx < 0 || idx >= THEME_COLOR_COUNT) idx = 0;
    g_themeColor = THEME_COLORS[idx].color;
    nvsPutInt("theme_idx", idx);
}

int themeColorIndex() {
    for (int i = 0; i < THEME_COLOR_COUNT; i++)
        if (THEME_COLORS[i].color == g_themeColor) return i;
    return 0;
}

const char *themeColorName() {
    return THEME_COLORS[themeColorIndex()].name;
}
