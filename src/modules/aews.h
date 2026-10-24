#pragma once
#include <TFT_eSPI.h>

// AEWS — Apocalypse Early Warning System (ported from CYD-Weather).
// Business-jet anomaly index from the Quantum-Meteor /ews route.
void aewsInit(TFT_eSPI &tft);
bool aewsLoop(TFT_eSPI &tft);   // false = return to home
void aewsWarmCache();           // pre-fetch at boot
