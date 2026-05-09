#pragma once
#include <TFT_eSPI.h>

void sysinfoInit(TFT_eSPI &tft);
bool sysinfoLoop(TFT_eSPI &tft);  // returns false when user requests home
