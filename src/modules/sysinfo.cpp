#include "sysinfo.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <WiFi.h>
#include <SD.h>
#include <time.h>
#include <esp_chip_info.h>

#define BOARD_SDCARD_CS  39
#define KB_ADDR          0x55
#define FW_VERSION       "v1.0"

// ── Data model ────────────────────────────────────────────────────────────────
struct SysInfo {
    uint32_t cpuMhz;
    float    tempC;
    uint32_t freeHeap;
    uint32_t totalHeap;
    uint32_t freePsram;
    uint32_t totalPsram;
    uint32_t uptimeSec;
    int      chipCores;
    int      chipRev;
    bool     wifiOk;
    char     wifiSsid[33];
    char     wifiIp[16];
    int      wifiRssi;
    bool     sdOk;
    uint64_t sdTotal;
    uint64_t sdUsed;
    char     serverUrl[48];
    int      personaSlot;
};

static SysInfo   s_info;
static TFT_eSPI *s_tft = nullptr;

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Gather all local data ─────────────────────────────────────────────────────
static void gatherData() {
    s_info.cpuMhz     = ESP.getCpuFreqMHz();
    s_info.freeHeap   = ESP.getFreeHeap();
    s_info.totalHeap  = ESP.getHeapSize();
    s_info.freePsram  = ESP.getFreePsram();
    s_info.totalPsram = ESP.getPsramSize();
    s_info.uptimeSec  = millis() / 1000;

    // temperatureRead() on ESP32-S3 may return 0 if unsupported by this core
    s_info.tempC = temperatureRead();

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    s_info.chipCores = ci.cores;
    s_info.chipRev   = ci.revision;

    s_info.wifiOk = (WiFi.status() == WL_CONNECTED);
    if (s_info.wifiOk) {
        strlcpy(s_info.wifiSsid, WiFi.SSID().c_str(), sizeof(s_info.wifiSsid));
        strlcpy(s_info.wifiIp,   WiFi.localIP().toString().c_str(), sizeof(s_info.wifiIp));
        s_info.wifiRssi = WiFi.RSSI();
    } else {
        strlcpy(s_info.wifiSsid, "NOT CONNECTED", sizeof(s_info.wifiSsid));
        strlcpy(s_info.wifiIp,   "---", sizeof(s_info.wifiIp));
        s_info.wifiRssi = 0;
    }

    s_info.sdOk = false;
    if (SD.begin(BOARD_SDCARD_CS)) {
        s_info.sdOk    = true;
        s_info.sdTotal = SD.totalBytes();
        s_info.sdUsed  = SD.usedBytes();
        SD.end();
    }

    String url = nvsGetString("server_url");
    strlcpy(s_info.serverUrl, url.isEmpty() ? "" : url.c_str(), sizeof(s_info.serverUrl));
    s_info.personaSlot = nvsGetInt("persona_slot", 0);

}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void formatUptime(uint32_t sec, char *out, int outLen) {
    uint32_t h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    if (h > 0)      snprintf(out, outLen, "%uh %02um %02us", h, m, s);
    else if (m > 0) snprintf(out, outLen, "%um %02us", m, s);
    else            snprintf(out, outLen, "%us", s);
}

static const char *rssiLabel(int rssi) {
    if (rssi >= -60) return "STRONG";
    if (rssi >= -70) return "GOOD";
    if (rssi >= -80) return "FAIR";
    if (rssi >= -90) return "WEAK";
    return "POOR";
}

static uint16_t rssiColor(int rssi) {
    if (rssi >= -60) return COL_CYAN;
    if (rssi >= -75) return COL_AMBER;
    return COL_RED;
}

// ── Inline progress bar ───────────────────────────────────────────────────────
static void drawBar(int x, int y, int w, int h, float pct, uint16_t col) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    s_tft->drawRect(x, y, w, h, COL_GREY_DIM);
    int filled = (int)(pct * (w - 2));
    if (filled > 0) s_tft->fillRect(x + 1, y + 1, filled, h - 2, col);
}

// ── Two-column stat row helpers ───────────────────────────────────────────────
static void drawLabel(int x, int y, const char *label) {
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_GREY_DIM, COL_BG);
    s_tft->drawString(label, x, y);
}

static void drawValue(int x, int y, const char *val, uint16_t col = COL_WHITE) {
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(col, COL_BG);
    s_tft->drawString(val, x, y);
}

static void divider(int &y) {
    y += 12;
    s_tft->drawFastHLine(0, y, SCREEN_W, COL_GREY_DIM);
    y += 4;
}

// ── Screen drawing ────────────────────────────────────────────────────────────
static void drawSysinfoScreen() {
    s_tft->fillScreen(COL_BG);

    // Topbar
    drawTopbar(*s_tft, "< HOME | SYSTEM", FW_VERSION, COL_CYAN);

    // Status bar
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_GREY_DIM, COL_BG);
    s_tft->drawString("AI TERMINAL", 4, TOPBAR_H + 3);
    const char *bdate = __DATE__;
    int bdw = s_tft->textWidth(bdate);
    s_tft->drawString(bdate, SCREEN_W - bdw - 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, COL_GREY_DIM);

    int y = TOPBAR_H + STATUSBAR_H + 4;

    // ── Section 1: CPU / uptime / temp ────────────────────────────────────────
    char upBuf[24];
    formatUptime(s_info.uptimeSec, upBuf, sizeof(upBuf));
    char cpuBuf[12];
    snprintf(cpuBuf, sizeof(cpuBuf), "%u MHz", s_info.cpuMhz);

    drawLabel(4, y, "CPU");   drawValue(46, y, cpuBuf);
    drawLabel(168, y, "UP");  drawValue(190, y, upBuf);

    y += 11;

    char tempBuf[14];
    if (s_info.tempC > 5.0f && s_info.tempC < 150.0f)
        snprintf(tempBuf, sizeof(tempBuf), "%.1f C", s_info.tempC);
    else
        strlcpy(tempBuf, "N/A", sizeof(tempBuf));

    char coreBuf[16];
    snprintf(coreBuf, sizeof(coreBuf), "%d core  r%d", s_info.chipCores, s_info.chipRev);

    drawLabel(4, y, "TEMP");  drawValue(46, y, tempBuf);
    drawLabel(168, y, "CHIP"); drawValue(200, y, coreBuf);

    divider(y);

    // ── Section 2: Memory ─────────────────────────────────────────────────────
    // HEAP — bar shows used, text shows free/total
    {
        char memBuf[20];
        snprintf(memBuf, sizeof(memBuf), "%uK / %uK",
                 s_info.freeHeap / 1024, s_info.totalHeap / 1024);
        int valW = s_tft->textWidth(memBuf);
        int bx   = 46, bw = SCREEN_W - bx - valW - 8;
        float usedPct = s_info.totalHeap > 0
            ? 1.0f - (float)s_info.freeHeap / s_info.totalHeap : 0;
        drawLabel(4, y, "HEAP");
        drawBar(bx, y, bw, 8, usedPct, usedPct < 0.8f ? COL_CYAN : COL_AMBER);
        drawValue(SCREEN_W - valW - 4, y, memBuf);
    }

    y += 11;

    // PSRAM
    drawLabel(4, y, "PSRM");
    if (s_info.totalPsram > 0) {
        char psBuf[20];
        snprintf(psBuf, sizeof(psBuf), "%uK / %uK",
                 s_info.freePsram / 1024, s_info.totalPsram / 1024);
        int valW = s_tft->textWidth(psBuf);
        int bx   = 46, bw = SCREEN_W - bx - valW - 8;
        float usedPct = 1.0f - (float)s_info.freePsram / s_info.totalPsram;
        drawBar(bx, y, bw, 8, usedPct, COL_CYAN);
        drawValue(SCREEN_W - valW - 4, y, psBuf);
    } else {
        drawValue(46, y, "NOT DETECTED", COL_GREY_DIM);
    }

    divider(y);

    // ── Section 3: WiFi ───────────────────────────────────────────────────────
    drawLabel(4, y, "WIFI");
    if (s_info.wifiOk) {
        drawValue(46, y, "CONNECTED", COL_CYAN);
        char rssiBuf[24];
        snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm  %s",
                 s_info.wifiRssi, rssiLabel(s_info.wifiRssi));
        int rw = s_tft->textWidth(rssiBuf);
        drawValue(SCREEN_W - rw - 4, y, rssiBuf, rssiColor(s_info.wifiRssi));
    } else {
        drawValue(46, y, "OFFLINE", COL_RED);
    }

    y += 11;
    drawLabel(4, y, "SSID"); drawValue(46, y, s_info.wifiSsid);

    y += 11;
    drawLabel(4, y, "IP");   drawValue(24, y, s_info.wifiIp);

    divider(y);

    // ── Section 4: SD card ────────────────────────────────────────────────────
    drawLabel(4, y, "SD");
    if (s_info.sdOk && s_info.sdTotal > 0) {
        float totalGB = (float)(s_info.sdTotal) / (1024.0f * 1024.0f * 1024.0f);
        float usedGB  = (float)(s_info.sdUsed)  / (1024.0f * 1024.0f * 1024.0f);
        char sdBuf[24];
        if (totalGB >= 1.0f)
            snprintf(sdBuf, sizeof(sdBuf), "%.1f / %.0f GB", usedGB, totalGB);
        else
            snprintf(sdBuf, sizeof(sdBuf), "%lu / %lu MB",
                     (uint32_t)(s_info.sdUsed / (1024 * 1024)),
                     (uint32_t)(s_info.sdTotal / (1024 * 1024)));
        int valW = s_tft->textWidth(sdBuf);
        int bx   = 22, bw = SCREEN_W - bx - valW - 8;
        float usedPct = (float)(s_info.sdUsed) / s_info.sdTotal;
        drawBar(bx, y, bw, 8, usedPct, usedPct < 0.85f ? COL_CYAN : COL_AMBER);
        drawValue(SCREEN_W - valW - 4, y, sdBuf);
    } else {
        drawValue(22, y, "NOT MOUNTED", COL_GREY_DIM);
    }

    divider(y);

    // ── Section 5: Config ─────────────────────────────────────────────────────
    drawLabel(4, y, "SERVER");
    {
        char urlTrunc[46];
        strlcpy(urlTrunc, s_info.serverUrl[0] ? s_info.serverUrl : "(not set)", sizeof(urlTrunc));
        drawValue(52, y, urlTrunc, s_info.serverUrl[0] ? COL_WHITE : COL_GREY_DIM);
    }

    y += 11;
    drawLabel(4, y, "PERSONA");
    char psBuf[12];
    snprintf(psBuf, sizeof(psBuf), "slot %d", s_info.personaSlot + 1);
    drawValue(54, y, psBuf);

    y += 11;
    // Current UTC time (read from NTP)
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        drawLabel(4, y, "UTC");
        char timeBuf[12];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        drawValue(30, y, timeBuf, COL_CYAN);
    }

    // Hint bar ─────────────────────────────────────────────────────────────
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_GREY_DIM, COL_BG);
    s_tft->drawCentreString("Q=home  R=refresh", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
}

// ── Public API ────────────────────────────────────────────────────────────────
void sysinfoInit(TFT_eSPI &tft) {
    s_tft = &tft;

    tft.fillScreen(COL_BG);
    gatherData();
    drawSysinfoScreen();
}

bool sysinfoLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }
    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;
    if (key == 'r' || key == 'R') {
        gatherData();
        drawSysinfoScreen();
    }
    delay(20);
    return true;
}
