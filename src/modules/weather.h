#pragma once
#include <TFT_eSPI.h>

void weatherInit(TFT_eSPI &tft);
bool weatherLoop(TFT_eSPI &tft);  // returns false when user requests home
