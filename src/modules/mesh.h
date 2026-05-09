#pragma once
#include <TFT_eSPI.h>

void meshInit(TFT_eSPI &tft);
bool meshLoop(TFT_eSPI &tft);  // returns false when user requests home
