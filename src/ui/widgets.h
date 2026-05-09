#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "theme.h"
#include "../config/nvs_config.h"

// ── Corner brackets (6px L-shape) ────────────────────────────────────────────
inline void drawCornerBrackets(TFT_eSPI &tft, int x, int y, int w, int h, uint16_t col, int len = 6) {
    // top-left
    tft.drawFastHLine(x,         y,         len, col);
    tft.drawFastVLine(x,         y,         len, col);
    // top-right
    tft.drawFastHLine(x + w - len, y,       len, col);
    tft.drawFastVLine(x + w - 1,   y,       len, col);
    // bottom-left
    tft.drawFastHLine(x,         y + h - 1, len, col);
    tft.drawFastVLine(x,         y + h - len, len, col);
    // bottom-right
    tft.drawFastHLine(x + w - len, y + h - 1, len, col);
    tft.drawFastVLine(x + w - 1,   y + h - len, len, col);
}

// ── Scanline effect (dim every other row inside a rect) ───────────────────────
inline void drawScanlines(TFT_eSPI &tft, int x, int y, int w, int h) {
    for (int row = y + 1; row < y + h; row += 2) {
        tft.drawFastHLine(x, row, w, 0x0841);  // very dim overlay
    }
}

// ── Top bar ───────────────────────────────────────────────────────────────────
inline void drawTopbar(TFT_eSPI &tft, const char *title, const char *right, uint16_t col) {
    tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_BG);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(col, COL_BG);
    tft.drawString(title, 4, 4);
    if (right && right[0]) {
        int rw = tft.textWidth(right);
        tft.setTextColor(col, COL_BG);
        tft.drawString(right, SCREEN_W - rw - 4, 4);
    }
    tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, col);
}

// ── Status bar ────────────────────────────────────────────────────────────────
inline void drawStatusBar(TFT_eSPI &tft, bool wifiOk, bool loraOk, const char *crewTag, int kp) {
    tft.fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    tft.setTextFont(FONT_SMALL);
    int x = 4;

    // WiFi dot
    tft.setTextColor(wifiOk ? COL_CYAN : COL_GREY_DIM, COL_BG);
    tft.drawString("WiFi \xB7", x, TOPBAR_H + 2);
    x += tft.textWidth("WiFi \xB7") + 6;

    // Crew tag
    if (crewTag && crewTag[0]) {
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.drawString(crewTag, x, TOPBAR_H + 2);
    }

    (void)kp;  // Kp removed from home status bar

    tft.drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, COL_CYAN);
}

// ── Bottom bar ────────────────────────────────────────────────────────────────
// T-Deck battery ADC is IO04. The board uses a divider, so the ADC millivolts
// are doubled back to an approximate LiPo pack voltage.
#ifndef TDECK_BAT_ADC_PIN
#define TDECK_BAT_ADC_PIN 4
#endif

inline int readTDeckBatteryMv() {
#if defined(ARDUINO_ARCH_ESP32)
    static bool adcReady = false;
    if (!adcReady) {
        analogReadResolution(12);
        analogSetPinAttenuation(TDECK_BAT_ADC_PIN, ADC_11db);
        adcReady = true;
    }
    uint32_t mv = analogReadMilliVolts(TDECK_BAT_ADC_PIN);
    if (mv < 100) return -1;
    return (int)(mv * 2);
#else
    return -1;
#endif
}

inline int batteryPercentFromMv(int mv) {
    if (mv <= 0) return -1;
    if (mv >= 4200) return 100;
    if (mv <= 3300) return 0;
    return (mv - 3300) * 100 / 900;
}

inline void drawBatteryIndicator(TFT_eSPI &tft, int x, int y, uint16_t fg = COL_CYAN, uint16_t bg = COL_BG) {
    int pct = batteryPercentFromMv(readTDeckBatteryMv());
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(fg, bg);

    char buf[6];
    if (pct < 0) strlcpy(buf, "--%", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%d%%", pct);
    tft.drawString(buf, x, y + 5);

    int iconX = x + tft.textWidth(buf) + 4;
    int iconY = y + 4;
    tft.drawRect(iconX, iconY, 17, 9, fg);
    tft.drawFastVLine(iconX + 17, iconY + 3, 3, fg);
    int fillW = pct < 0 ? 0 : (pct * 13) / 100;
    if (fillW > 0) tft.fillRect(iconX + 2, iconY + 2, fillW, 5, fg);
}

inline void drawBatteryIndicatorRight(TFT_eSPI &tft, int y, uint16_t fg = COL_CYAN, uint16_t bg = COL_BG) {
    int pct = batteryPercentFromMv(readTDeckBatteryMv());
    tft.setTextFont(FONT_SMALL);

    char buf[6];
    if (pct < 0) strlcpy(buf, "--%", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%d%%", pct);

    int iconW = 18;
    int gap = 4;
    int textW = tft.textWidth(buf);
    int x = SCREEN_W - 4 - iconW - gap - textW;
    drawBatteryIndicator(tft, x, y, fg, bg);
}

inline String footerDisplayName() {
    String name = nvsGetString("display_name", "Commander Smoke");
    name.trim();
    if (name.isEmpty()) name = "Commander Smoke";
    if (name.length() > 18) name = name.substring(0, 18);
    return name;
}

inline void drawFooterName(TFT_eSPI &tft, int y, uint16_t fg = COL_CYAN, uint16_t bg = COL_BG) {
    String name = footerDisplayName();
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(fg, bg);
    tft.drawString(name, 4, y + 3);
}
inline void drawBottomBar(TFT_eSPI &tft, const char *left, bool blinkState) {
    int y = SCREEN_H - BOTTOMBAR_H;
    tft.fillRect(0, y, SCREEN_W, BOTTOMBAR_H, COL_BG);
    tft.drawFastHLine(0, y, SCREEN_W, COL_CYAN);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(COL_CYAN, COL_BG);
    if (left && left[0]) tft.drawString(left, 4, y + 3);
    else drawFooterName(tft, y);
    drawBatteryIndicatorRight(tft, y + 1);
}

// ── Status dot ────────────────────────────────────────────────────────────────
inline void drawStatusDot(TFT_eSPI &tft, int x, int y, uint16_t col) {
    tft.fillCircle(x, y, 3, col);
}
