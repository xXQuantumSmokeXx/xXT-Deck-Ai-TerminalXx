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
    TILE_AEWS    = 7,   // Apocalypse Early Warning System (jet anomalies)
    TILE_OUTAGES = 8,   // US power outages, national + local 24 h
    TILE_ORACLE  = 9,   // Oracle divination (implementation pending)
    TILE_ROAD    = 10,  // News: RSS feed aggregator
    TILE_SYSINFO = 11,  // System info
    TILE_COUNT   = 12
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
