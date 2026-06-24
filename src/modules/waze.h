#pragma once
#include <TFT_eSPI.h>

// Three focused Waze alert modules — one shared implementation, different filters.

void wazeHazardInit(TFT_eSPI &tft);   // ACCIDENT + HAZARD alerts
bool wazeHazardLoop(TFT_eSPI &tft);

void wazePoliceInit(TFT_eSPI &tft);   // POLICE alerts (shows subtypes)
bool wazePoliceLoop(TFT_eSPI &tft);

void wazeRoadInit(TFT_eSPI &tft);     // ROAD_CLOSED + JAM alerts
bool wazeRoadLoop(TFT_eSPI &tft);

void wazeTrackballUp();
void wazeTrackballDown();
void wazeWarmCache();           // pre-fetch TomTom hazards + Waze alerts at boot
