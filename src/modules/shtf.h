#pragma once
#include <TFT_eSPI.h>

void shtfInit(TFT_eSPI &tft);
bool shtfLoop(TFT_eSPI &tft);
void shtfTrackballUp();
void shtfTrackballDown();
void shtfWarmCache();           // pre-fetch + cache to NVS at boot
