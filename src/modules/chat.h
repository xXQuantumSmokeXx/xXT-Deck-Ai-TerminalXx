#pragma once
#include <TFT_eSPI.h>

void chatInit(TFT_eSPI &tft);
bool chatLoop(TFT_eSPI &tft);   // returns false when user requests home
void chatTrackballUp();
void chatTrackballDown();
void chatExit();                 // called when user returns to home
