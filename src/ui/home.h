#pragma once
#include <TFT_eSPI.h>

// Tile IDs (order matches the 2×4 grid, row-major)
enum TileID {
    TILE_CHAT    = 0,
    TILE_WEATHER = 1,
    TILE_SOLAR   = 2,
    TILE_ALERTS  = 3,
    TILE_BTC     = 4,
    TILE_FIRE    = 5,
    TILE_WORLD   = 6,
    TILE_SYSINFO = 7,
    TILE_COUNT   = 8
};

void homeInit(TFT_eSPI &tft);
void homeDraw(TFT_eSPI &tft);
void homeNavUp(TFT_eSPI &tft);
void homeNavDown(TFT_eSPI &tft);
void homeNavLeft(TFT_eSPI &tft);
void homeNavRight(TFT_eSPI &tft);
TileID homeSelected();
void homeSetWifiStatus(bool ok);
void homeSetLoraStatus(bool ok);
void homeSetKp(int kp);
void homeTick(TFT_eSPI &tft);  // call every loop; handles clock + blink
