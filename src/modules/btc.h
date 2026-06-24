#pragma once
#include <TFT_eSPI.h>

void btcInit(TFT_eSPI &tft);
bool btcLoop(TFT_eSPI &tft);  // returns false when user requests home
void btcWarmCache();           // pre-fetch + cache to SD at boot
