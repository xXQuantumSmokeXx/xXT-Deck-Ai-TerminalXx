#pragma once
#include <TFT_eSPI.h>

// OUTAGES — US power outages (ported from CYD-Weather).
// National + caller's-state 24 h series from the Quantum-Meteor /grid route.
void outagesInit(TFT_eSPI &tft);
bool outagesLoop(TFT_eSPI &tft);   // false = return to home
void outagesWarmCache();           // pre-fetch at boot
