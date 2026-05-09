#include "home.h"
#include "theme.h"
#include "widgets.h"
#include <Arduino.h>
#include <time.h>

// â”€â”€ Tile definitions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
struct TileDef {
    const char *label;
    uint16_t    accent;
};

static const TileDef TILES[TILE_COUNT] = {
    { "AI CHAT",  COL_MAGENTA },
    { "WEATHER",  COL_CYAN    },
    { "SOLAR",    COL_AMBER   },
    { "LOG",      COL_CYAN    },
    { "BTC",      COL_GOLD    },
    { "FIRES",    COL_RED     },
    { "QUAKES",   COL_GREEN   },
    { "SYSTEM",   COL_BLUE    },
};

// â”€â”€ State â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static int  s_cursor   = 0;
static bool s_wifiOk   = false;
static bool s_loraOk   = false;
static int  s_kp       = 0;
static int  s_lastMin  = -1;
static char s_clockStr[10] = "12:00 AM";

// â”€â”€ Helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void updateClock() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(s_clockStr, sizeof(s_clockStr), "%d:%02d %s", h, ti.tm_min, ap);
    }
}

// â”€â”€ Vector tile icons â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void drawTileIcon(TFT_eSPI &t, int idx, int cx, int cy) {
    const uint16_t C = COL_CYAN;
    const uint16_t B = COL_BG;

    switch (idx) {

    case 0: { // AI CHAT â€” speech bubble
        t.drawRoundRect(cx-13, cy-11, 26, 18, 4, C);
        // Tail pointing down-left
        t.fillTriangle(cx-13, cy+4, cx-13, cy+10, cx-4, cy+4, C);
        // Re-draw left edge of bubble (tail fill overlaps corner)
        t.drawFastVLine(cx-13, cy-7, 12, C);
        // Text lines inside bubble
        t.drawFastHLine(cx-8, cy-5, 16, C);
        t.drawFastHLine(cx-8, cy,   10, C);
        break;
    }

    case 1: { // WEATHER â€” sun peeking behind cloud
        // Sun outline (upper-right of icon)
        int sx = cx+7, sy = cy-9;
        t.drawCircle(sx, sy, 5, C);
        t.drawFastHLine(sx-9, sy, 3, C);
        t.drawFastHLine(sx+6, sy, 3, C);
        t.drawFastVLine(sx, sy-9, 3, C);
        t.drawFastVLine(sx, sy+6, 3, C);
        // Cloud (filled) â€” covers lower portion including part of sun
        t.fillCircle(cx-8, cy+4, 7, C);
        t.fillCircle(cx+1, cy,   9, C);
        t.fillCircle(cx+9, cy+4, 6, C);
        t.fillRect(cx-15, cy+4, 24, 9, C);
        break;
    }

    case 2: { // SOLAR â€” sun with rays
        t.fillCircle(cx, cy, 6, C);
        // Cardinal rays
        t.drawFastHLine(cx-13, cy, 5, C);
        t.drawFastHLine(cx+8,  cy, 5, C);
        t.drawFastVLine(cx, cy-13, 5, C);
        t.drawFastVLine(cx, cy+8,  5, C);
        // Diagonal rays (45Â° offsets â‰ˆ 0.707 Ã— r)
        t.drawLine(cx+6, cy-6, cx+10, cy-10, C);
        t.drawLine(cx-6, cy-6, cx-10, cy-10, C);
        t.drawLine(cx+6, cy+6, cx+10, cy+10, C);
        t.drawLine(cx-6, cy+6, cx-10, cy+10, C);
        break;
    }

    case 3: { // LOG - field note page
        t.drawRoundRect(cx-10, cy-13, 20, 26, 2, C);
        t.drawFastHLine(cx-6, cy-6, 12, C);
        t.drawFastHLine(cx-6, cy, 12, C);
        t.drawFastHLine(cx-6, cy+6, 8, C);
        t.fillTriangle(cx+5, cy-13, cx+10, cy-8, cx+5, cy-8, C);
        break;
    }

    case 4: { // BTC â€” coin with B
        t.drawCircle(cx, cy, 13, C);
        t.drawCircle(cx, cy, 12, C);   // double ring
        // B vertical stroke
        t.fillRect(cx-4, cy-8, 3, 16, C);
        // B horizontals
        t.fillRect(cx-4, cy-8, 8, 2, C);
        t.fillRect(cx-4, cy-1, 7, 2, C);
        t.fillRect(cx-4, cy+7, 8, 2, C);
        // B right bumps
        t.fillRect(cx+1, cy-6, 3, 5, C);
        t.fillRect(cx+1, cy+1, 4, 6, C);
        // Top/bottom tick serifs
        t.drawFastVLine(cx-2, cy-10, 2, C);
        t.drawFastVLine(cx-2, cy+9,  2, C);
        break;
    }

    case 5: { // FIRES â€” flame silhouette
        // Outer flame fill
        t.fillTriangle(cx, cy-13, cx-9, cy+4, cx+9, cy+4, C);
        t.fillCircle(cx-4, cy+3, 7, C);
        t.fillCircle(cx+4, cy+3, 7, C);
        t.fillRect(cx-11, cy+3, 22, 8, C);
        // Inner hollow
        t.fillTriangle(cx, cy-4, cx-4, cy+4, cx+4, cy+4, B);
        t.fillCircle(cx, cy+4, 4, B);
        break;
    }

    case 6: { // QUAKES â€” seismograph waveform
        int bl = cy + 3;  // baseline y
        t.drawFastHLine(cx-14, bl, 7, C);
        t.drawLine(cx-7, bl, cx-3, cy-11, C);
        t.drawLine(cx-3, cy-11, cx+2, cy+11, C);
        t.drawLine(cx+2, cy+11, cx+6, bl,   C);
        t.drawFastHLine(cx+6, bl, 8, C);
        break;
    }

    case 7: { // SYSTEM â€” gear
        t.drawCircle(cx, cy, 10, C);
        t.drawCircle(cx, cy,  5, C);
        // N/S/E/W teeth
        t.fillRect(cx-2, cy-14, 4, 5, C);
        t.fillRect(cx-2, cy+10, 4, 5, C);
        t.fillRect(cx-14, cy-2, 5, 4, C);
        t.fillRect(cx+10, cy-2, 5, 4, C);
        // Diagonal teeth
        t.fillRect(cx+6,  cy-11, 4, 4, C);
        t.fillRect(cx-10, cy-11, 4, 4, C);
        t.fillRect(cx+6,  cy+8,  4, 4, C);
        t.fillRect(cx-10, cy+8,  4, 4, C);
        break;
    }

    default: break;
    }
}

// â”€â”€ Draw a single tile â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void drawTile(TFT_eSPI &tft, int idx, bool selected) {
    int col = idx % TILE_COLS;
    int row = idx / TILE_COLS;
    int x   = col * TILE_W;
    int y   = CONTENT_Y + row * TILE_H;
    int w   = TILE_W;
    int h   = TILE_H;

    tft.fillRect(x, y, w, h, COL_BG);

    // Icon centered in the non-label area
    int icx = x + w / 2;
    int icy = y + (h - 18) / 2;
    drawTileIcon(tft, idx, icx, icy);

    // Label
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(selected ? COL_WHITE : COL_GREY_MID, COL_BG);
    int labelW = tft.textWidth(TILES[idx].label);
    tft.drawString(TILES[idx].label, x + (w - labelW) / 2, y + h - 18);

    // Selection indicator
    if (selected) {
        drawCornerBrackets(tft, x + 1, y + 1, w - 2, h - 2, COL_CYAN, 8);
    }
}

// â”€â”€ Public API â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
void homeInit(TFT_eSPI &tft) {
    s_cursor = 0;
    tft.fillScreen(COL_BG);
    homeDraw(tft);
}

void homeDraw(TFT_eSPI &tft) {
    drawTopbar(tft, ">> T-Deck-Ai-Terminal", s_clockStr, COL_CYAN);
    drawStatusBar(tft, s_wifiOk, s_loraOk, "READY", s_kp);
    for (int i = 0; i < TILE_COUNT; i++) drawTile(tft, i, i == s_cursor);
    drawBottomBar(tft, "", false);
}

static void moveCursor(TFT_eSPI &tft, int next) {
    int prev = s_cursor;
    s_cursor = next;
    drawTile(tft, prev, false);
    drawTile(tft, next, true);
    drawBottomBar(tft, "", false);
}

void homeNavUp(TFT_eSPI &tft)    { int n = s_cursor - TILE_COLS; if (n >= 0) moveCursor(tft, n); }
void homeNavDown(TFT_eSPI &tft)  { int n = s_cursor + TILE_COLS; if (n < TILE_COUNT) moveCursor(tft, n); }
void homeNavLeft(TFT_eSPI &tft)  { if (s_cursor % TILE_COLS > 0) moveCursor(tft, s_cursor - 1); }
void homeNavRight(TFT_eSPI &tft) { if (s_cursor % TILE_COLS < TILE_COLS - 1) moveCursor(tft, s_cursor + 1); }

TileID homeSelected() { return (TileID)s_cursor; }

void homeSetWifiStatus(bool ok) { s_wifiOk = ok; }
void homeSetLoraStatus(bool ok) { s_loraOk = ok; }
void homeSetKp(int kp)          { s_kp = kp; }

void homeTick(TFT_eSPI &tft) {
    struct tm ti;
    if (getLocalTime(&ti, 0) && ti.tm_min != s_lastMin) {
        s_lastMin = ti.tm_min;
        updateClock();
        drawTopbar(tft, ">> T-Deck-Ai-Terminal", s_clockStr, COL_CYAN);
    }
}
