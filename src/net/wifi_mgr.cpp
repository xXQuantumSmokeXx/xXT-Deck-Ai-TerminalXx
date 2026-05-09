#include "wifi_mgr.h"
#include "../config/nvs_config.h"
#include <WiFi.h>
#include <SD.h>

#define BOARD_SDCARD_CS 39

bool wifiConnect() {
    String ssid, pass;
    if (!wifiLoadCreds(ssid, pass) || ssid.isEmpty()) return false;
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
    return WiFi.status() == WL_CONNECTED;
}

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String wifiIP() {
    return WiFi.localIP().toString();
}

void wifiSaveCreds(const String &ssid, const String &pass) {
    nvsPutString("wifi_ssid", ssid);
    nvsPutString("wifi_pass", pass);
}

bool wifiLoadCreds(String &ssid, String &pass) {
    ssid = nvsGetString("wifi_ssid");
    pass = nvsGetString("wifi_pass");
    return ssid.length() > 0;
}

bool wifiLoadFromSD(String &ssid, String &pass) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/wifi.txt")) { SD.end(); return false; }
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) { SD.end(); return false; }
    ssid = f.readStringUntil('\n'); ssid.trim();
    pass = f.readStringUntil('\n'); pass.trim();
    f.close();
    SD.remove("/wifi.txt");
    SD.end();
    return ssid.length() > 0;
}
