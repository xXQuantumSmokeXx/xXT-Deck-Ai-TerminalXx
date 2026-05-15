#include "weather.h"
#include "../ui/theme.h"
#include <math.h>
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>

#define BOARD_SDCARD_CS  39
#define KB_ADDR        0x55
#define CACHE_TTL_SEC  1800   // 30 minutes
#define DEFAULT_LAT    39.9526f
#define DEFAULT_LON   -75.1652f
#define DEFAULT_CITY   "PHILADELPHIA, PA"

// ── Weather data ──────────────────────────────────────────────────────────────
struct WeatherData {
    float    tempF;
    float    feelsLikeF;
    int      code;
    float    windMph;
    int      humidity;
    float    pressureHpa;
    float    visibilityMi;
    // 5-day daily forecast
    int      dayCode[5];
    float    dayHiF[5];
    float    dayLoF[5];
    char     dayName[5][4];   // "Mon", "Tue", etc.
    // Meta
    bool     valid;
    bool     fromCache;
    char     syncTime[10];    // "HH:MM" of last fetch
    char     locationName[32];
};

static WeatherData s_wx;
static TFT_eSPI   *s_tft = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────
static float celsiusToF(float c) { return c * 9.0f / 5.0f + 32.0f; }
static float kmhToMph(float k)   { return k * 0.621371f; }
static float mToMi(float m)      { return m * 0.000621371f; }

// Vector weather icon centered at (cx,cy). sz=1 → small (forecast), sz=2 → large.
static void drawWxIcon(int cx, int cy, int code, uint16_t C, int sz) {
    if (code == 0) {
        // Clear — sun with 8 rays
        s_tft->drawCircle(cx, cy, 4*sz, C);
        s_tft->drawFastHLine(cx-8*sz, cy, 2*sz+1, C);
        s_tft->drawFastHLine(cx+5*sz, cy, 2*sz+1, C);
        s_tft->drawFastVLine(cx, cy-8*sz, 2*sz+1, C);
        s_tft->drawFastVLine(cx, cy+5*sz, 2*sz+1, C);
        s_tft->drawLine(cx+3*sz, cy-3*sz, cx+5*sz, cy-5*sz, C);
        s_tft->drawLine(cx-3*sz, cy-3*sz, cx-5*sz, cy-5*sz, C);
        s_tft->drawLine(cx+3*sz, cy+3*sz, cx+5*sz, cy+5*sz, C);
        s_tft->drawLine(cx-3*sz, cy+3*sz, cx-5*sz, cy+5*sz, C);
    } else if (code <= 2) {
        // Mostly clear — sun peeking behind cloud
        s_tft->drawCircle(cx+5*sz, cy-5*sz, 3*sz, C);
        s_tft->fillCircle(cx-4*sz, cy+2*sz, 3*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-1*sz, 5*sz, C);
        s_tft->fillCircle(cx+7*sz, cy+2*sz, 3*sz, C);
        s_tft->fillRect(cx-7*sz, cy+2*sz, 14*sz, 4*sz, C);
    } else if (code == 3) {
        // Overcast — cloud
        s_tft->fillCircle(cx-5*sz, cy+1*sz, 4*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-2*sz, 5*sz, C);
        s_tft->fillCircle(cx+7*sz, cy+1*sz, 3*sz, C);
        s_tft->fillRect(cx-9*sz, cy+1*sz, 16*sz, 5*sz, C);
    } else if (code <= 48) {
        // Fog — horizontal lines
        s_tft->drawFastHLine(cx-9*sz, cy-4*sz, 18*sz, C);
        s_tft->drawFastHLine(cx-9*sz, cy,       18*sz, C);
        s_tft->drawFastHLine(cx-9*sz, cy+4*sz, 18*sz, C);
        s_tft->drawFastHLine(cx-6*sz, cy+8*sz, 12*sz, C);
    } else if (code <= 67) {
        // Rain/drizzle — cloud + drops
        s_tft->fillCircle(cx-4*sz, cy-2*sz, 3*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-5*sz, 4*sz, C);
        s_tft->fillCircle(cx+6*sz, cy-2*sz, 3*sz, C);
        s_tft->fillRect(cx-7*sz, cy-2*sz, 13*sz, 4*sz, C);
        s_tft->drawFastVLine(cx-4*sz, cy+4*sz, 4*sz, C);
        s_tft->drawFastVLine(cx+1*sz, cy+6*sz, 4*sz, C);
        s_tft->drawFastVLine(cx+5*sz, cy+4*sz, 4*sz, C);
    } else if (code <= 77) {
        // Snow — cloud + snowflake crosses
        s_tft->fillCircle(cx-4*sz, cy-2*sz, 3*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-5*sz, 4*sz, C);
        s_tft->fillCircle(cx+6*sz, cy-2*sz, 3*sz, C);
        s_tft->fillRect(cx-7*sz, cy-2*sz, 13*sz, 4*sz, C);
        s_tft->drawFastHLine(cx-7*sz, cy+5*sz, 4*sz, C);
        s_tft->drawFastVLine(cx-5*sz, cy+3*sz, 4*sz, C);
        s_tft->drawFastHLine(cx-1*sz, cy+7*sz, 4*sz, C);
        s_tft->drawFastVLine(cx+1*sz, cy+5*sz, 4*sz, C);
        s_tft->drawFastHLine(cx+5*sz, cy+5*sz, 4*sz, C);
        s_tft->drawFastVLine(cx+7*sz, cy+3*sz, 4*sz, C);
    } else if (code <= 82) {
        // Showers — cloud + angled drops
        s_tft->fillCircle(cx-4*sz, cy-2*sz, 3*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-5*sz, 4*sz, C);
        s_tft->fillCircle(cx+6*sz, cy-2*sz, 3*sz, C);
        s_tft->fillRect(cx-7*sz, cy-2*sz, 13*sz, 4*sz, C);
        s_tft->drawLine(cx-5*sz, cy+3*sz, cx-7*sz, cy+7*sz, C);
        s_tft->drawLine(cx-1*sz, cy+3*sz, cx-3*sz, cy+7*sz, C);
        s_tft->drawLine(cx+3*sz, cy+3*sz, cx+1*sz, cy+7*sz, C);
        s_tft->drawLine(cx+7*sz, cy+3*sz, cx+5*sz, cy+7*sz, C);
    } else {
        // Storm — cloud + lightning bolt
        s_tft->fillCircle(cx-4*sz, cy-2*sz, 3*sz, C);
        s_tft->fillCircle(cx+1*sz, cy-5*sz, 4*sz, C);
        s_tft->fillCircle(cx+6*sz, cy-2*sz, 3*sz, C);
        s_tft->fillRect(cx-7*sz, cy-2*sz, 13*sz, 4*sz, C);
        s_tft->drawLine(cx+2*sz, cy+2*sz,  cx-2*sz, cy+7*sz,  C);
        s_tft->drawLine(cx-2*sz, cy+7*sz,  cx+2*sz, cy+11*sz, C);
        if (sz >= 2) {
            s_tft->drawLine(cx+3*sz, cy+2*sz,  cx-1*sz, cy+7*sz,  C);
            s_tft->drawLine(cx-1*sz, cy+7*sz,  cx+3*sz, cy+11*sz, C);
        }
    }
}

static const char *weatherDesc(int code) {
    if (code == 0)          return "CLEAR";
    if (code == 1)          return "MOSTLY CLEAR";
    if (code == 2)          return "PARTLY CLOUDY";
    if (code == 3)          return "OVERCAST";
    if (code <= 48)         return "FOG";
    if (code <= 55)         return "DRIZZLE";
    if (code <= 67)         return "RAIN";
    if (code <= 77)         return "SNOW";
    if (code <= 82)         return "RAIN SHOWERS";
    if (code <= 86)         return "SNOW SHOWERS";
    if (code == 95)         return "THUNDERSTORM";
    return "SEVERE STORM";
}

static bool isAlertCondition(int code) {
    return code >= 95 || (code >= 75 && code <= 77);  // storm or heavy snow
}

// Day-of-week from ISO date string "YYYY-MM-DD"
static void dateToDayName(const char *iso, char *out, int outLen) {
    int y, m, d;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) != 3) { strlcpy(out, "???", outLen); return; }
    // Tomohiko Sakamoto's algorithm
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y--;
    int dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    static const char *names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    strlcpy(out, names[dow], outLen);
}

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Blocking readLine (for setlocation prompt) ────────────────────────────────
static String wxReadLine(const String &prompt) {
    String buf = "";
    s_tft->fillScreen(COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_ORANGE, COL_BG);
    s_tft->drawString(prompt, 2, 10);
    auto redraw = [&]() {
        s_tft->fillRect(0, 26, SCREEN_W, 16, COL_BG);
        s_tft->setTextColor(COL_WHITE, COL_BG);
        s_tft->drawString(buf + "_", 2, 28);
    };
    redraw();
    while (true) {
        char k = readKeyboard();
        if (k == 0) { delay(20); continue; }
        if (k == '\r' || k == '\n') break;
        if ((k == 8 || k == 127) && buf.length() > 0) buf.remove(buf.length() - 1);
        else if (isprint((unsigned char)k) && buf.length() < 40) buf += k;
        redraw();
        delay(20);
    }
    return buf;
}

// ── SD cache ─────────────────────────────────────────────────────────────────
static bool saveCacheToSD(const String &json) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/cache")) SD.mkdir("/cache");
    File f = SD.open("/cache/weather.json", FILE_WRITE);
    if (!f) { SD.end(); return false; }
    f.print(json);
    f.close();
    SD.end();
    nvsPutInt("wx_cached_at", (int)time(nullptr));
    return true;
}

static bool loadCacheFromSD(String &json) {
    int cachedAt = nvsGetInt("wx_cached_at", 0);
    if (cachedAt == 0) return false;
    time_t now = time(nullptr);
    if (now > 1000000 && (now - cachedAt) > CACHE_TTL_SEC) return false;

    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/cache/weather.json")) { SD.end(); return false; }
    File f = SD.open("/cache/weather.json", FILE_READ);
    if (!f) { SD.end(); return false; }
    json = "";
    while (f.available()) json += (char)f.read();
    f.close();
    SD.end();
    return json.length() > 10;
}

// ── JSON parser ───────────────────────────────────────────────────────────────
static bool parseWeatherJson(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    JsonObject cur = doc["current"];
    if (cur.isNull()) return false;

    s_wx.tempF       = celsiusToF(cur["temperature_2m"].as<float>());
    s_wx.feelsLikeF  = celsiusToF(cur["apparent_temperature"].as<float>());
    s_wx.code        = cur["weathercode"].as<int>();
    s_wx.windMph     = kmhToMph(cur["windspeed_10m"].as<float>());
    s_wx.humidity    = cur["relativehumidity_2m"].as<int>();
    s_wx.pressureHpa = cur["surface_pressure"].as<float>();
    s_wx.visibilityMi= mToMi(cur["visibility"].as<float>());

    JsonObject daily = doc["daily"];
    if (!daily.isNull()) {
        JsonArray times = daily["time"].as<JsonArray>();
        JsonArray codes = daily["weathercode"].as<JsonArray>();
        JsonArray maxT  = daily["temperature_2m_max"].as<JsonArray>();
        JsonArray minT  = daily["temperature_2m_min"].as<JsonArray>();
        for (int i = 0; i < 5; i++) {
            s_wx.dayCode[i] = codes[i].as<int>();
            s_wx.dayHiF[i]  = celsiusToF(maxT[i].as<float>());
            s_wx.dayLoF[i]  = celsiusToF(minT[i].as<float>());
            const char *iso = times[i].as<const char *>();
            dateToDayName(iso ? iso : "1970-01-01", s_wx.dayName[i], 4);
        }
    }

    // Sync time for display
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(s_wx.syncTime, sizeof(s_wx.syncTime), "%d:%02d %s", h, ti.tm_min, ap);
    } else {
        strlcpy(s_wx.syncTime, "--:--", sizeof(s_wx.syncTime));
    }

    s_wx.valid = true;
    return true;
}

// ── Fetch from network ────────────────────────────────────────────────────────
static bool fetchWeather() {
    float lat = nvsGetString("wx_lat").toFloat();
    float lon = nvsGetString("wx_lon").toFloat();
    if (lat == 0.0f && lon == 0.0f) { lat = DEFAULT_LAT; lon = DEFAULT_LON; }

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weathercode,windspeed_10m,"
        "relativehumidity_2m,surface_pressure,visibility,apparent_temperature"
        "&daily=weathercode,temperature_2m_max,temperature_2m_min"
        "&timezone=auto&forecast_days=5",
        lat, lon);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String json = http.getString();
    http.end();

    if (!parseWeatherJson(json)) return false;
    saveCacheToSD(json);
    s_wx.fromCache = false;
    return true;
}

// ── Try cache then network ────────────────────────────────────────────────────
static void loadWeatherData() {
    s_wx.valid = false;

    String json;
    if (loadCacheFromSD(json) && parseWeatherJson(json)) {
        s_wx.fromCache = true;
        return;
    }

    if (WiFi.isConnected()) fetchWeather();
    // If still not valid, s_wx.valid stays false - show NO DATA
}

// ── Location name derivation (simple lat/lon → region label) ─────────────────
static void buildLocationName() {
    String latStr = nvsGetString("wx_lat");
    String lonStr = nvsGetString("wx_lon");
    if (latStr.isEmpty() && lonStr.isEmpty()) {
        strlcpy(s_wx.locationName, DEFAULT_CITY, sizeof(s_wx.locationName));
        return;
    }
    float lat = latStr.isEmpty() ? DEFAULT_LAT : latStr.toFloat();
    float lon = lonStr.isEmpty() ? DEFAULT_LON : lonStr.toFloat();
    snprintf(s_wx.locationName, sizeof(s_wx.locationName),
             "%.2f N  %.2f W", fabsf(lat), fabsf(lon));
}

// ── Screen drawing ────────────────────────────────────────────────────────────
static void spiReinitForTFT() {
    digitalWrite(BOARD_SDCARD_CS, HIGH);
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
    SPI.endTransaction();
}

static void drawWeatherScreen() {
    s_tft->fillScreen(COL_BG);

    // Topbar
    char rightBuf[12] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(rightBuf, sizeof(rightBuf), "%d:%02d %s", h, ti.tm_min, ap);
    }
    drawTopbar(*s_tft, "< HOME | WEATHER", rightBuf, g_themeColor);

    // Status bar: location left, sync time right
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(s_wx.locationName, 4, TOPBAR_H + 3);
    if (s_wx.valid) {
        char syncBuf[24];
        snprintf(syncBuf, sizeof(syncBuf), "%s %s",
                 s_wx.fromCache ? "CACHED" : "LIVE", s_wx.syncTime);
        int sw = s_tft->textWidth(syncBuf);
        s_tft->setTextColor(s_wx.fromCache ? COL_AMBER : g_themeColor, COL_BG);
        s_tft->drawString(syncBuf, SCREEN_W - sw - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    // fillScreen is unreliable after SD SPI ops — secondary fillRect clears what it missed.
    s_tft->fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, COL_BG);

    int y = TOPBAR_H + STATUSBAR_H + 4;  // content start

    if (!s_wx.valid) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, y + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("OFFLINE - no cache available", SCREEN_W / 2, y + 55, FONT_SMALL);
        s_tft->drawCentreString("Q=home  R=retry  L=set location", SCREEN_W / 2, SCREEN_H - 18, FONT_SMALL);
        return;
    }

    // ── Big temperature block (left 2/3) ──────────────────────────────────────
    char tempBuf[8];
    snprintf(tempBuf, sizeof(tempBuf), "%d", (int)roundf(s_wx.tempF));
    s_tft->setTextFont(FONT_LARGE);
    s_tft->setTextColor(COL_WHITE, COL_BG);
    s_tft->drawString(tempBuf, 6, y);

    // Degree F label next to temp
    int tempW = s_tft->textWidth(tempBuf);
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("F", 6 + tempW + 2, y + 4);

    // Condition icon (right side of temp block) — vector, sz=2
    drawWxIcon(SCREEN_W * 2 / 3 + 22, y + 18, s_wx.code, g_themeColor, 2);

    // Condition text
    int y2 = y + 30;
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(weatherDesc(s_wx.code), 6, y2);

    // Feels like
    int y3 = y2 + 18;
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    char flBuf[20];
    snprintf(flBuf, sizeof(flBuf), "feels like %d F", (int)roundf(s_wx.feelsLikeF));
    s_tft->drawString(flBuf, 6, y3);

    // ── Stats row ─────────────────────────────────────────────────────────────
    int ys = y3 + 14;
    s_tft->setTextFont(FONT_SMALL);
    // 4 columns across 320px → 80px each
    struct { const char *label; char val[16]; } stats[4];
    snprintf(stats[0].val, 16, "%d mph",  (int)roundf(s_wx.windMph));   stats[0].label = "WIND";
    snprintf(stats[1].val, 16, "%d%%",    s_wx.humidity);                stats[1].label = "HUMID";
    snprintf(stats[2].val, 16, "%d hPa", (int)roundf(s_wx.pressureHpa)); stats[2].label = "BARO";
    snprintf(stats[3].val, 16, "%.1fmi",  s_wx.visibilityMi);            stats[3].label = "VIS";

    for (int i = 0; i < 4; i++) {
        int x = i * 80 + 4;
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(stats[i].label, x, ys);
        s_tft->setTextColor(COL_WHITE, COL_BG);
        s_tft->drawString(stats[i].val, x, ys + 10);
    }

    // ── Divider ───────────────────────────────────────────────────────────────
    int yd = ys + 24;
    s_tft->drawFastHLine(0, yd, SCREEN_W, g_themeColor);

    // ── 5-day forecast strip ─────────────────────────────────────────────────
    // Icons are sz=2 for visibility. Layout fills to near-bottom so the
    // hint bar can live at a fixed SCREEN_H - BOTTOMBAR_H position.
    int yf   = yd + 8;
    int colW = SCREEN_W / 5;  // 64px each
    for (int i = 0; i < 5; i++) {
        int x = i * colW + (colW / 2);  // column center x

        // Day name (today = cyan, others = grey)
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        int lw = s_tft->textWidth(s_wx.dayName[i]);
        s_tft->drawString(s_wx.dayName[i], x - lw / 2, yf + 1);

        // Icon — sz=2 for larger display; center at yf+26 clears day-name text
        drawWxIcon(x, yf + 32, s_wx.dayCode[i], g_themeColor, 2);

        // Hi / lo temps below icon
        s_tft->setTextFont(FONT_SMALL);
        char hi[8], lo[8];
        snprintf(hi, sizeof(hi), "%d", (int)roundf(s_wx.dayHiF[i]));
        snprintf(lo, sizeof(lo), "%d", (int)roundf(s_wx.dayLoF[i]));
        int hw  = s_tft->textWidth(hi);
        int lw2 = s_tft->textWidth(lo);
        s_tft->setTextColor(COL_WHITE, COL_BG);
        s_tft->drawString(hi, x - hw  / 2, yf + 60);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(lo, x - lw2 / 2, yf + 72);

        // Vertical divider between columns (except last)
        if (i < 4) s_tft->drawFastVLine(x + colW / 2, yf - 1, 90, g_themeColor);
    }

    // Horizontal separator at forecast bottom
    s_tft->drawFastHLine(0, yf + 88, SCREEN_W, g_themeColor);

    // ── Alert / hint bar — pinned to screen bottom ────────────────────────────
    int ya = SCREEN_H - BOTTOMBAR_H;
    if (isAlertCondition(s_wx.code)) {
        s_tft->fillRect(0, ya, SCREEN_W, BOTTOMBAR_H, COL_AMBER);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_BG, COL_AMBER);
        const char *alertText = s_wx.code >= 95
            ? "! STORM WARNING - SEEK SHELTER"
            : "* HEAVY SNOW - TRAVEL HAZARD";
        int aw = s_tft->textWidth(alertText);
        s_tft->drawString(alertText, (SCREEN_W - aw) / 2, ya + 3);
    } else {
        s_tft->drawFastHLine(0, ya, SCREEN_W, g_themeColor);
        s_tft->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, g_themeColor);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("Q=home  R=refresh  L=location", SCREEN_W / 2, ya + 3, FONT_SMALL);
        drawBatteryIndicatorRight(*s_tft, ya + 1);
    }
}

// ── Set location prompt ───────────────────────────────────────────────────────
static void doSetLocation() {
    String latStr = wxReadLine("Latitude (e.g. 41.2033):");
    latStr.trim();
    if (latStr.isEmpty()) return;
    String lonStr = wxReadLine("Longitude (e.g. -74.7640):");
    lonStr.trim();
    if (lonStr.isEmpty()) return;
    nvsPutString("wx_lat", latStr);
    nvsPutString("wx_lon", lonStr);
    nvsPutInt("wx_cached_at", 0);  // invalidate cache
    buildLocationName();
}

// ── Public API ────────────────────────────────────────────────────────────────
void weatherInit(TFT_eSPI &tft) {
    s_tft = &tft;
    s_wx.valid = false;

    buildLocationName();  // NVS only — no SD

    // Show loading screen BEFORE any SD operations so the TFT clear runs
    // while the SPI bus is in a known TFT state (mirrors world.cpp pattern).
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | WEATHER", "", g_themeColor);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("Fetching weather data...", SCREEN_W / 2, SCREEN_H / 2, FONT_SMALL);

    // SD ops: check cache
    String json;
    bool cacheHit = loadCacheFromSD(json) && parseWeatherJson(json);
    if (cacheHit) s_wx.fromCache = true;

    // Restore SPI for TFT after SD operations
    spiReinitForTFT();

    if (cacheHit) {
        drawWeatherScreen();
        return;
    }

    // No cache — fetch live (fetchWeather → saveCacheToSD → SD ops), then draw
    if (WiFi.isConnected()) {
        fetchWeather();
        spiReinitForTFT();
    }
    drawWeatherScreen();
}

bool weatherLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("wx_cached_at", 0);
        loadWeatherData();    // SD ops inside
        spiReinitForTFT();
        drawWeatherScreen();
    }

    if (key == 'l' || key == 'L') {
        doSetLocation();
        loadWeatherData();    // SD ops inside
        spiReinitForTFT();
        drawWeatherScreen();
    }

    delay(20);
    return true;
}
