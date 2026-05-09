#pragma once
#include <TFT_eSPI.h>

void worldInit(TFT_eSPI &tft);
void worldInitFires(TFT_eSPI &tft);
bool worldLoop(TFT_eSPI &tft);  // returns false when user requests home
