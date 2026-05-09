#pragma once
#include <TFT_eSPI.h>

void solarInit(TFT_eSPI &tft);
bool solarLoop(TFT_eSPI &tft);  // returns false when user requests home
