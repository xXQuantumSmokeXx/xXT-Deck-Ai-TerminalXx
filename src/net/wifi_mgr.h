#pragma once
#include <Arduino.h>

bool wifiConnect();           // connect using stored creds; returns true if connected
bool wifiIsConnected();
String wifiIP();

// Credential management (called from setup flow)
void wifiSaveCreds(const String &ssid, const String &pass);
bool wifiLoadCreds(String &ssid, String &pass);

// SD bootstrap: load /wifi.txt, delete it, save to NVS
bool wifiLoadFromSD(String &ssid, String &pass);
