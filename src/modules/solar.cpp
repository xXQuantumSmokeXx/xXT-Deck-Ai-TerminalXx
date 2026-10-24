#include "solar.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>

// One request to the Quantum-Meteor /solar route replaces the five SWPC
// products and the DONKI CME lookup this screen used to pull itself. Two of
// those SWPC paths (plasma-2-hour, mag-2-hour) have since been retired with a
// 404, and DONKI's api.nasa.gov door answers 503 — the Worker already reads
// the paths that still work. The 48 h Kp forecast has no Worker route and is
// still fetched from SWPC directly; it is optional, so a failure costs the
// forecast strip and nothing else.

#define KB_ADDR        0x55
#define CACHE_TTL_SEC  3600  // 1 hour

// ── Data model ────────────────────────────────────────────────────────────────
struct SolarData {
    float kpCurrent;
    float kpHistory[8];
    struct { char hhmm[6]; float kp; } forecast[7];
    int   forecastCount;
    float windSpeedKms;
    float bzNT;
    float densityPcc;
    float xrayFlux;
    char  xrayClass[6];
    char  flareClass[8];    // "M1.5", "X2.1", "NONE"
    char  flareTime[14];    // "05-08 03:52"
    float cmeSpeedKms;
    char  cmeTime[14];      // "05-08 04:12", "NONE"
    // Meta
    bool  valid;
    bool  fromCache;
    time_t fetchedAt;       // drives the standard LIVE/CACHED stamp
};

static SolarData s_sol;
static TFT_eSPI *s_tft = nullptr;

// ── Kp helpers ────────────────────────────────────────────────────────────────
static const char *kpCondition(float kp) {
    if (kp < 3)  return "QUIET";
    if (kp < 4)  return "UNSETTLED";
    if (kp < 5)  return "ACTIVE";
    if (kp < 6)  return "G1 MINOR";
    if (kp < 7)  return "G2 MODERATE";
    if (kp < 8)  return "G3 STRONG";
    if (kp < 9)  return "G4 SEVERE";
    return "G5 EXTREME";
}

static int kpGLevel(float kp) {
    if (kp >= 9) return 5;
    if (kp >= 8) return 4;
    if (kp >= 7) return 3;
    if (kp >= 6) return 2;
    if (kp >= 5) return 1;
    return 0;
}

static int kpAuroraLat(float kp) {
    static const int lats[] = {90, 80, 75, 70, 65, 60, 55, 50, 45, 40};
    int i = (int)kp;
    if (i < 0) i = 0;
    if (i > 9) i = 9;
    return lats[i];
}

static uint16_t kpColor(float kp) {
    if (kp < 3) return g_themeColor;
    if (kp < 5) return COL_AMBER;
    return COL_RED;
}

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Worker payload ────────────────────────────────────────────────────────────
// /solar: kp, hist[8], wind, dens, temp, bz, bt, xray, xclass, flare,
//         flareTime, cme, cmeTime — 237 bytes as of 2026-10-23.
static bool fetchFromWorker() {
    JsonDocument doc;
    if (!httpJson(QM_BASE "/solar", doc, 12000)) return false;

    float kp = doc["kp"] | 0.0f;
    if (kp <= 0.0f) return false;          // keep the previous screen, not zeros

    s_sol.kpCurrent    = kp;
    s_sol.windSpeedKms = doc["wind"] | 0.0f;
    s_sol.densityPcc   = doc["dens"] | 0.0f;
    s_sol.bzNT         = doc["bz"]   | 0.0f;
    s_sol.xrayFlux     = doc["xray"] | 0.0f;
    s_sol.cmeSpeedKms  = doc["cme"]  | 0.0f;

    strlcpy(s_sol.xrayClass,  doc["xclass"]    | "N/A",  sizeof(s_sol.xrayClass));
    strlcpy(s_sol.flareClass, doc["flare"]     | "NONE", sizeof(s_sol.flareClass));
    strlcpy(s_sol.flareTime,  doc["flareTime"] | "---",  sizeof(s_sol.flareTime));
    strlcpy(s_sol.cmeTime,    doc["cmeTime"]   | "NONE", sizeof(s_sol.cmeTime));

    int n = 0;
    for (JsonVariant v : doc["hist"].as<JsonArray>()) {
        if (n >= 8) break;
        s_sol.kpHistory[n++] = v | 0.0f;
    }
    while (n < 8) s_sol.kpHistory[n++] = kp;
    return true;
}

// ── Kp forecast — the one remaining direct SWPC read ─────────────────────────
static bool readKpValue(JsonVariant row, float &kp) {
    JsonVariant v;
    if (row.is<JsonArray>()) {
        v = row[1];
    } else {
        v = row["Kp"];
        if (v.isNull()) v = row["kp"];
    }
    if (v.isNull()) return false;
    if (v.is<float>() || v.is<int>()) { kp = v.as<float>(); return true; }
    const char *s = v.as<const char *>();
    if (!s) return false;
    kp = atof(s);
    return true;
}

static const char *readTimeTag(JsonVariant row) {
    if (row.is<JsonArray>()) return row[0] | "";
    return row["time_tag"] | "";
}

static bool fetchKpForecast() {
    JsonDocument doc;
    if (!httpJson("https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json",
                  doc, 10000)) return false;

    JsonArray arr = doc.as<JsonArray>();
    s_sol.forecastCount = 0;

    // Two passes: predicted rows first, then observed ones as filler.
    for (int pass = 0; pass < 2 && s_sol.forecastCount < 7; pass++) {
        for (int i = 0; i < (int)arr.size() && s_sol.forecastCount < 7; i++) {
            JsonVariant row = arr[i];
            const char *observed = row.is<JsonArray>() ? "" : (row["observed"] | "");
            bool isFuture = observed[0] && strcasecmp(observed, "observed") != 0;
            if ((pass == 0 && !isFuture) || (pass == 1 && isFuture)) continue;

            float kp = 0.0f;
            if (!readKpValue(row, kp)) continue;
            const char *timeTag = readTimeTag(row);
            if (timeTag && strlen(timeTag) >= 16) {
                snprintf(s_sol.forecast[s_sol.forecastCount].hhmm, 6, "%.5s", timeTag + 11);
                s_sol.forecast[s_sol.forecastCount].kp = kp;
                s_sol.forecastCount++;
            }
        }
    }
    return s_sol.forecastCount > 0;
}

// ── Fetch all data ─────────────────────────────────────────────────────────────
static void fetchAllData() {
    s_sol.valid = false;
    s_sol.windSpeedKms = 0; s_sol.bzNT = 0;
    s_sol.densityPcc = 0;   s_sol.xrayFlux = 0;
    strlcpy(s_sol.xrayClass,  "N/A",  sizeof(s_sol.xrayClass));
    strlcpy(s_sol.flareClass, "NONE", sizeof(s_sol.flareClass));
    strlcpy(s_sol.flareTime,  "---",  sizeof(s_sol.flareTime));
    s_sol.cmeSpeedKms = 0;
    strlcpy(s_sol.cmeTime, "NONE", sizeof(s_sol.cmeTime));

    if (!WiFi.isConnected()) return;

    if (!fetchFromWorker()) return;        // no worker, no screen — the cache covers this
    fetchKpForecast();                     // optional; forecast strip degrades alone

    s_sol.valid     = true;
    s_sol.fromCache = false;
    s_sol.fetchedAt = time(nullptr);

    nvsPutInt("sol_cached_at", (int)time(nullptr));
    nvsPutInt("sol_kp_cur",    (int)(s_sol.kpCurrent    * 100));
    nvsPutInt("sol_bz",        (int)(s_sol.bzNT         * 100));
    nvsPutInt("sol_wind",      (int)s_sol.windSpeedKms);
    nvsPutInt("sol_dens",      (int)(s_sol.densityPcc   * 100));
    nvsPutString("sol_xray",   s_sol.xrayClass);
    nvsPutString("sol_flare",  s_sol.flareClass);
    nvsPutString("sol_flaret", s_sol.flareTime);
    nvsPutInt("sol_cme_spd",   (int)s_sol.cmeSpeedKms);
    nvsPutString("sol_cmet",   s_sol.cmeTime);
    nvsPutInt("sol_hist_n", 8);
    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sol_hist%d", i);
        nvsPutInt(key, (int)(s_sol.kpHistory[i] * 100));
    }
    nvsPutInt("sol_fc_n", s_sol.forecastCount);
    for (int i = 0; i < 7; i++) {
        char keyKp[16];
        char keyTime[16];
        snprintf(keyKp, sizeof(keyKp), "sol_fc%d", i);
        snprintf(keyTime, sizeof(keyTime), "sol_fct%d", i);
        nvsPutInt(keyKp, (int)(s_sol.forecast[i].kp * 100));
        nvsPutString(keyTime, s_sol.forecast[i].hhmm);
    }
}

// ── Load from NVS cache ────────────────────────────────────────────────────────
static bool loadFromCache() {
    int cachedAt = nvsGetInt("sol_cached_at", 0);
    if (cachedAt == 0) return false;
    time_t now = time(nullptr);
    if (now > 1000000 && (now - cachedAt) > CACHE_TTL_SEC) return false;

    s_sol.kpCurrent    = nvsGetInt("sol_kp_cur", 0) / 100.0f;
    if (s_sol.kpCurrent <= 0.0f) return false;

    s_sol.bzNT         = nvsGetInt("sol_bz",     0) / 100.0f;
    s_sol.windSpeedKms = nvsGetInt("sol_wind",   0);
    s_sol.densityPcc   = nvsGetInt("sol_dens",   0) / 100.0f;

    String xray = nvsGetString("sol_xray");
    strlcpy(s_sol.xrayClass, xray.isEmpty() ? "---" : xray.c_str(), sizeof(s_sol.xrayClass));

    String flare = nvsGetString("sol_flare");
    strlcpy(s_sol.flareClass, flare.isEmpty() ? "NONE" : flare.c_str(), sizeof(s_sol.flareClass));

    String flareT = nvsGetString("sol_flaret");
    strlcpy(s_sol.flareTime, flareT.isEmpty() ? "---" : flareT.c_str(), sizeof(s_sol.flareTime));

    s_sol.cmeSpeedKms = nvsGetInt("sol_cme_spd", 0);

    String cmeT = nvsGetString("sol_cmet");
    strlcpy(s_sol.cmeTime, cmeT.isEmpty() ? "NONE" : cmeT.c_str(), sizeof(s_sol.cmeTime));

    int histN = nvsGetInt("sol_hist_n", 0);
    if (histN < 8) return false;
    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sol_hist%d", i);
        s_sol.kpHistory[i] = histN > i ? nvsGetInt(key, 0) / 100.0f : s_sol.kpCurrent;
    }

    // The forecast strip is optional — a cache without it is still useful.
    int fcN = nvsGetInt("sol_fc_n", 0);
    if (fcN < 0) fcN = 0;
    if (fcN > 7) fcN = 7;
    s_sol.forecastCount = fcN;
    for (int i = 0; i < 7; i++) {
        char keyKp[16];
        char keyTime[16];
        snprintf(keyKp, sizeof(keyKp), "sol_fc%d", i);
        snprintf(keyTime, sizeof(keyTime), "sol_fct%d", i);
        s_sol.forecast[i].kp = i < fcN ? nvsGetInt(keyKp, 0) / 100.0f : 0.0f;
        String hhmm = nvsGetString(keyTime);
        strlcpy(s_sol.forecast[i].hhmm, (i < fcN && !hhmm.isEmpty()) ? hhmm.c_str() : "--:--",
                sizeof(s_sol.forecast[i].hhmm));
    }

    s_sol.valid     = true;
    s_sol.fromCache = true;
    s_sol.fetchedAt = (time_t)cachedAt;
    return true;
}

// ── Screen drawing ────────────────────────────────────────────────────────────
static void drawSolarScreen() {
    s_tft->fillScreen(COL_BG);

    char rightBuf[12] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(rightBuf, sizeof(rightBuf), "%d:%02d %s", h, ti.tm_min, ap);
    }
    drawTopbar(*s_tft, "< HOME | SOLAR", rightBuf, g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    if (s_sol.valid) {
        char stamp[20];
        fmtStamp(s_sol.fromCache, s_sol.fetchedAt, stamp, sizeof(stamp));
        s_tft->setTextColor(s_sol.fromCache ? COL_AMBER : g_themeColor, COL_BG);
        s_tft->drawString(stamp, 4, TOPBAR_H + 3);
        char kpLbl[24];
        snprintf(kpLbl, sizeof(kpLbl), "Kp %.1f  %s", s_sol.kpCurrent, kpCondition(s_sol.kpCurrent));
        int w = s_tft->textWidth(kpLbl);
        s_tft->setTextColor(kpColor(s_sol.kpCurrent), COL_BG);
        s_tft->drawString(kpLbl, SCREEN_W - w - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int cy = CONTENT_Y + 2;

    if (!s_sol.valid) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, cy + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("OFFLINE - no cache available", SCREEN_W / 2, cy + 52, FONT_SMALL);
        drawMenuBar(*s_tft, "Q=HOME  R=RETRY");
        return;
    }

    // ── Left: big Kp ─────────────────────────────────────────────────────────
    uint16_t kpCol = kpColor(s_sol.kpCurrent);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("Kp INDEX", 4, cy);

    char kpBuf[6];
    snprintf(kpBuf, sizeof(kpBuf), "%.1f", s_sol.kpCurrent);
    s_tft->setTextFont(FONT_LARGE);
    s_tft->setTextColor(kpCol, COL_BG);
    s_tft->drawString(kpBuf, 4, cy + 10);

    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(kpCol, COL_BG);
    s_tft->drawString(kpCondition(s_sol.kpCurrent), 4, cy + 38);

    char auroraLbl[16];
    snprintf(auroraLbl, sizeof(auroraLbl), "AURORA ~%dN", kpAuroraLat(s_sol.kpCurrent));
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(auroraLbl, 4, cy + 48);

    // ── Right: 6 stats at 9px spacing ────────────────────────────────────────
    int rx = 154;
    struct { const char *lbl; char val[18]; uint16_t col; } stats[6];

    bool windValid = s_sol.windSpeedKms > 0;
    if (windValid) snprintf(stats[0].val, 18, "%.0f km/s", s_sol.windSpeedKms);
    else           strlcpy(stats[0].val, "---", 18);
    stats[0].lbl = "WIND"; stats[0].col = COL_WHITE;

    bool bzValid = windValid || s_sol.bzNT != 0.0f;
    if (bzValid) snprintf(stats[1].val, 18, s_sol.bzNT < 0 ? "%.1f nT v" : "%.1f nT ^", s_sol.bzNT);
    else         strlcpy(stats[1].val, "---", 18);
    stats[1].lbl = "Bz"; stats[1].col = (!bzValid) ? COL_GREY_MID : (s_sol.bzNT < 0 ? COL_RED : g_themeColor);

    bool densValid = s_sol.densityPcc > 0;
    if (densValid) snprintf(stats[2].val, 18, "%.1f p/cc", s_sol.densityPcc);
    else           strlcpy(stats[2].val, "---", 18);
    stats[2].lbl = "DENS"; stats[2].col = COL_WHITE;

    snprintf(stats[3].val, 18, "%s",          s_sol.xrayClass);
    stats[3].lbl = "XRAY"; stats[3].col = COL_AMBER;

    snprintf(stats[4].val, 18, "%s",          s_sol.flareClass);
    stats[4].lbl = "FLR"; stats[4].col = COL_AMBER;

    if (s_sol.cmeSpeedKms > 0)
        snprintf(stats[5].val, 18, "%.0f km/s", s_sol.cmeSpeedKms);
    else
        strlcpy(stats[5].val, s_sol.cmeTime, 18);
    stats[5].lbl = "CME"; stats[5].col = COL_RED;

    for (int i = 0; i < 6; i++) {
        int sy = cy + i * 9;
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(stats[i].lbl, rx, sy);
        s_tft->setTextColor(stats[i].col, COL_BG);
        s_tft->drawString(stats[i].val, rx + 32, sy);
    }

    // -- 24h Kp bar chart -----------------------------------------------------
    int bottomY = SCREEN_H - BOTTOMBAR_H;
    int chartY = cy + 58;
    s_tft->drawFastHLine(0, chartY - 2, SCREEN_W, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("24H Kp", 4, chartY);

    int barAreaY = chartY + 10;
    int barMaxH  = 34;
    int barW     = 35;
    int barGap   = 3;
    int barStartX = (SCREEN_W - 8 * (barW + barGap) + barGap) / 2;
    int histLabelY = barAreaY + barMaxH + 2;

    for (int i = 0; i < 8; i++) {
        float kp = s_sol.kpHistory[i];
        int barH = (int)(kp / 9.0f * barMaxH);
        if (barH < 2) barH = 2;
        int x = barStartX + i * (barW + barGap);
        int y = barAreaY + barMaxH - barH;
        uint16_t bc = kpColor(kp);
        s_tft->drawRect(x, barAreaY, barW, barMaxH, g_themeColor);
        s_tft->fillRect(x + 1, y, barW - 2, barH, bc);
        char kpN[4]; snprintf(kpN, sizeof(kpN), "%.0f", kp);
        int lw = s_tft->textWidth(kpN);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(kpN, x + (barW - lw) / 2, histLabelY);
    }
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("9", barStartX - 8, barAreaY);
    s_tft->drawString("0", barStartX - 8, barAreaY + barMaxH - 6);

    // -- 48h Kp forecast ------------------------------------------------------
    int foreY = histLabelY + 11;
    s_tft->drawFastHLine(0, foreY - 2, SCREEN_W, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("48H FORECAST", 4, foreY);

    int count = min(s_sol.forecastCount, 7);
    if (count > 0) {
        int cellW = SCREEN_W / count;
        int timeY = foreY + 10;
        int fBarY = timeY + 10;
        int fBarH = barMaxH;
        int forecastLabelY = fBarY + fBarH + 2;
        for (int i = 0; i < count; i++) {
            int x = i * cellW;
            int cx = x + cellW / 2;
            float kp = s_sol.forecast[i].kp;
            uint16_t fc = kpColor(kp);
            int barH = (int)(kp / 9.0f * fBarH);
            if (barH < 2) barH = 2;

            s_tft->drawRect(x + 4, fBarY, cellW - 8, fBarH, g_themeColor);
            s_tft->fillRect(x + 5, fBarY + fBarH - barH, cellW - 10, barH - 1, fc);

            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(g_themeColor, COL_BG);
            int tw = s_tft->textWidth(s_sol.forecast[i].hhmm);
            s_tft->drawString(s_sol.forecast[i].hhmm, cx - tw / 2, timeY);

            char kpN[5]; snprintf(kpN, sizeof(kpN), "%.1f", kp);
            s_tft->setTextColor(fc, COL_BG);
            int kw = s_tft->textWidth(kpN);
            s_tft->drawString(kpN, cx - kw / 2, forecastLabelY);

            if (i < count - 1) s_tft->drawFastVLine(x + cellW - 1, foreY + 1, bottomY - foreY - 2, g_themeColor);
        }
    } else {
        s_tft->setTextColor(COL_GREY_MID, COL_BG);
        s_tft->drawString("forecast unavailable", 4, foreY + 12);
    }

    // ── Alert / hint bar — fixed at screen bottom ─────────────────────────────
    int g = kpGLevel(s_sol.kpCurrent);
    bool bzAlert = s_sol.bzNT < -5.0f && s_sol.bzNT != 0.0f;

    if (g >= 3) {
        s_tft->fillRect(0, bottomY, SCREEN_W, BOTTOMBAR_H, COL_RED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_WHITE, COL_RED);
        char msg[40]; snprintf(msg, sizeof(msg), "! G%d STORM WATCH - COMMS DEGRADED", g);
        int mw = s_tft->textWidth(msg);
        s_tft->drawString(msg, (SCREEN_W - mw) / 2, bottomY + 3);
    } else if (g >= 1) {
        s_tft->fillRect(0, bottomY, SCREEN_W, BOTTOMBAR_H, COL_AMBER);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_BG, COL_AMBER);
        char msg[40]; snprintf(msg, sizeof(msg), "G%d GEOMAGNETIC STORM ACTIVE", g);
        int mw = s_tft->textWidth(msg);
        s_tft->drawString(msg, (SCREEN_W - mw) / 2, bottomY + 3);
    } else if (bzAlert) {
        s_tft->fillRect(0, bottomY, SCREEN_W, BOTTOMBAR_H, COL_AMBER);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_BG, COL_AMBER);
        s_tft->drawCentreString("Bz SOUTHWARD - LoRa IMPACT POSSIBLE", SCREEN_W / 2, bottomY + 3, FONT_SMALL);
    } else {
        drawMenuBar(*s_tft, "Q=HOME  R=REFRESH");
    }
}

// ── Boot-time cache warming ──────────────────────────────────────────────────
// Called once at startup before the user navigates to any screen.
// Populates NVS cache so solarInit() always finds data ready.
void solarWarmCache() {
    if (!WiFi.isConnected()) return;
    fetchAllData();
}

// ── Public API ────────────────────────────────────────────────────────────────
void solarInit(TFT_eSPI &tft) {
    s_tft = &tft;
    memset(&s_sol, 0, sizeof(s_sol));
    strlcpy(s_sol.xrayClass,  "N/A",  sizeof(s_sol.xrayClass));
    strlcpy(s_sol.flareClass, "NONE", sizeof(s_sol.flareClass));
    strlcpy(s_sol.flareTime,  "---",  sizeof(s_sol.flareTime));
    strlcpy(s_sol.cmeTime,    "NONE", sizeof(s_sol.cmeTime));

    if (loadFromCache()) {
        drawSolarScreen();
        return;
    }

    // Show loading indicator only when no valid cache is available.
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | SOLAR", "", g_themeColor);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("Fetching solar data...", SCREEN_W / 2, SCREEN_H / 2, FONT_SMALL);

    if (WiFi.isConnected()) fetchAllData();
    if (!s_sol.valid) loadFromCache();

    drawSolarScreen();
}

bool solarLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("sol_cached_at", 0);
        fetchAllData();
        drawSolarScreen();
    }

    delay(20);
    return true;
}
