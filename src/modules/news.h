#pragma once
#include <TFT_eSPI.h>

void newsInit(TFT_eSPI &tft);
bool newsLoop(TFT_eSPI &tft);
void newsTrackballUp();
void newsTrackballDown();
void newsWarmCache();           // pre-fetch + cache to NVS at boot
