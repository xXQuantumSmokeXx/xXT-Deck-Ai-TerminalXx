#pragma once
#include <TFT_eSPI.h>
#include "theme.h"

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

    tft.drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, COL_GREY_DIM);
}

// ── Bottom bar ────────────────────────────────────────────────────────────────
inline void drawBottomBar(TFT_eSPI &tft, const char *left, bool blinkState) {
    int y = SCREEN_H - BOTTOMBAR_H;
    tft.fillRect(0, y, SCREEN_W, BOTTOMBAR_H, COL_BG);
    tft.drawFastHLine(0, y, SCREEN_W, COL_GREY_DIM);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(COL_CYAN, COL_BG);
    if (left && left[0]) tft.drawString(left, 4, y + 3);
}

// ── Status dot ────────────────────────────────────────────────────────────────
inline void drawStatusDot(TFT_eSPI &tft, int x, int y, uint16_t col) {
    tft.fillCircle(x, y, 3, col);
}
