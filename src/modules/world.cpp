#include "world.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#define KB_ADDR    0x55
#define MAX_ITEMS  12
#define ROW_H      16

// ── Data models ───────────────────────────────────────────────────────────────
struct QuakeItem {
    float mag;
    char  place[72];
    char  when[15];
};

struct FireItem {
    char  title[72];
    char  when[15];
};

static QuakeItem  s_quakes[MAX_ITEMS];
static int        s_quakeCount   = 0;
static FireItem   s_fires[MAX_ITEMS];
static int        s_fireCount    = 0;
static bool       s_showingFires = false;
static char       s_syncStr[12]  = "";
static TFT_eSPI  *s_tft          = nullptr;

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Helpers ───────────────────────────────────────────────────────────────────
static void updateSyncStr() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(s_syncStr, sizeof(s_syncStr), "%d:%02d %s", h, ti.tm_min, ap);
    }
}

static void truncStr(const char *src, char *dst, int maxChars) {
    if (!src || maxChars <= 0) { dst[0] = '\0'; return; }
    int len = (int)strlen(src);
    if (len <= maxChars) {
        strlcpy(dst, src, (size_t)maxChars + 1);
    } else {
        strlcpy(dst, src, (size_t)maxChars + 1);
        dst[maxChars - 2] = '.';
        dst[maxChars - 1] = '.';
        dst[maxChars]     = '\0';
    }
}

static String fitTextPx(const char *src, int maxPx) {
    if (!src || maxPx <= 0) return String("");
    String out(src);
    if (!s_tft || s_tft->textWidth(out) <= maxPx) return out;

    while (out.length() > 2) {
        out.remove(out.length() - 1);
        String candidate = out + "..";
        if (s_tft->textWidth(candidate) <= maxPx) return candidate;
    }
    return String("..");
}

static void msToWhen(long long ms, char *out, int outLen) {
    time_t t = (time_t)(ms / 1000LL);
    struct tm *ti = gmtime(&t);
    if (ti) {
        int h = ti->tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(out, outLen, "%02d-%02d %d:%02d%s",
                 ti->tm_mon + 1, ti->tm_mday, h, ti->tm_min, ap);
    } else {
        strlcpy(out, "--", (size_t)outLen);
    }
}

static uint16_t magColor(float mag) {
    if (mag >= 7.0f) return COL_RED;
    if (mag >= 6.0f) return COL_AMBER;
    if (mag >= 5.0f) return COL_GOLD;
    return COL_CYAN;
}

// ── Quake fetch — USGS FDSNWS query (12 most recent M3.5+) ───────────────────
static bool fetchQuakes() {
    s_quakeCount = 0;

    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.begin(cl,
        "https://earthquake.usgs.gov/fdsnws/event/1/query"
        "?format=geojson&minmagnitude=3.5&limit=12&orderby=time");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "T-Deck-AI/1.0");

    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    JsonDocument filt;
    filt["features"][0]["properties"]["mag"]   = true;
    filt["features"][0]["properties"]["place"] = true;
    filt["features"][0]["properties"]["time"]  = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filt));
    if (err) return false;

    for (JsonObject f : doc["features"].as<JsonArray>()) {
        if (s_quakeCount >= MAX_ITEMS) break;
        JsonObject p = f["properties"];
        if (p.isNull()) continue;

        float       mag   = p["mag"].isNull()   ? 0.0f : p["mag"].as<float>();
        const char *place = p["place"].isNull() ? "Unknown" : p["place"].as<const char *>();
        long long   ms    = p["time"].isNull()  ? 0LL : p["time"].as<long long>();

        QuakeItem &q = s_quakes[s_quakeCount++];
        q.mag = mag;
        truncStr(place ? place : "Unknown", q.place, (int)sizeof(q.place) - 1);
        if (ms > 0) msToWhen(ms, q.when, (int)sizeof(q.when));
        else        strlcpy(q.when, "--", sizeof(q.when));
    }
    return s_quakeCount > 0;
}

// ── Fire fetch — NASA EONET open wildfire events ──────────────────────────────
// EONET is more reliable than ArcGIS WFIGS and returns clean JSON with fire
// name + date. HTTP/1.0 avoids chunked encoding for stream-safe ArduinoJson parse.
static bool fetchFires() {
    s_fireCount = 0;

    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.begin(cl,
        "https://eonet.gsfc.nasa.gov/api/v3/events"
        "?category=wildfires&status=open&limit=12");
    http.setTimeout(15000);
    http.addHeader("User-Agent", "T-Deck-AI/1.0");

    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument filt;
    filt["events"][0]["title"]                   = true;
    filt["events"][0]["geometry"][0]["date"]     = true;

    JsonDocument doc;
    WiFiClient *stream = http.getStreamPtr();
    DeserializationError err = deserializeJson(doc, *stream, DeserializationOption::Filter(filt));
    http.end();
    if (err) return false;

    JsonArray events = doc["events"].as<JsonArray>();
    if (events.isNull()) return false;

    for (JsonObject ev : events) {
        if (s_fireCount >= MAX_ITEMS) break;

        const char *title = ev["title"].isNull() ? "Unknown Fire" : ev["title"].as<const char *>();
        const char *date  = ev["geometry"][0]["date"].isNull() ? "" : ev["geometry"][0]["date"].as<const char *>();

        FireItem &fi = s_fires[s_fireCount++];
        truncStr(title, fi.title, (int)sizeof(fi.title) - 1);

        // Date: "2025-05-07T12:00:00Z" → "05-07"
        if (date && strlen(date) >= 10)
            snprintf(fi.when, sizeof(fi.when), "%.5s", date + 5);
        else
            strlcpy(fi.when, "--", sizeof(fi.when));
    }
    return s_fireCount > 0;
}

// ── Shared screen header ──────────────────────────────────────────────────────
static void drawWorldHeader(const char *title, const char *countLabel, int count) {
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    if (count > 0) {
        char buf[28];
        snprintf(buf, sizeof(buf), "%s: %d", countLabel, count);
        int w = s_tft->textWidth(buf);
        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawString(buf, SCREEN_W - w - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, COL_GREY_DIM);
    drawTopbar(*s_tft, title, s_syncStr, COL_CYAN);
}

// ── Quakes screen ─────────────────────────────────────────────────────────────
static void drawQuakesScreen() {
    s_tft->fillScreen(COL_BG);
    drawWorldHeader("< HOME | USGS", "M3.5+ 30-DAY", s_quakeCount);

    int cy = CONTENT_Y;

    if (s_quakeCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, cy + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawCentreString(!WiFi.isConnected() ? "OFFLINE" : "No M3.5+ quakes this month",
                                SCREEN_W / 2, cy + 52, FONT_SMALL);
        s_tft->drawCentreString("R=refresh  Q=home", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
        return;
    }

    int limit = min(s_quakeCount, (int)(CONTENT_H / ROW_H));
    for (int i = 0; i < limit; i++) {
        int y = cy + i * ROW_H;
        QuakeItem &q = s_quakes[i];
        uint16_t mc = magColor(q.mag);

        char magBuf[8];
        snprintf(magBuf, sizeof(magBuf), "M%.1f", q.mag);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(mc, COL_BG);
        s_tft->drawString(magBuf, 2, y + 2);

        int magW = s_tft->textWidth(magBuf) + 4;
        int dw = s_tft->textWidth(q.when);
        int dateX = SCREEN_W - dw - 2;
        int placeX = 2 + magW;
        String place = fitTextPx(q.place, dateX - placeX - 4);
        s_tft->setTextColor(COL_WHITE, COL_BG);
        s_tft->drawString(place, placeX, y + 2);

        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawString(q.when, dateX, y + 2);

        if (i < limit - 1)
            s_tft->drawFastHLine(0, y + ROW_H - 1, SCREEN_W, COL_GREY_DIM);
    }

    int by = SCREEN_H - BOTTOMBAR_H;
    s_tft->fillRect(0, by, SCREEN_W, BOTTOMBAR_H, COL_BG);
    s_tft->drawFastHLine(0, by, SCREEN_W, COL_CYAN);
    s_tft->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, COL_CYAN);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_CYAN, COL_BG);
    s_tft->drawCentreString("R=refresh  Q=home", SCREEN_W / 2, by + 3, FONT_SMALL);
}

// ── Fires screen ──────────────────────────────────────────────────────────────
static void drawFiresScreen() {
    s_tft->fillScreen(COL_BG);
    drawWorldHeader("< HOME | FIRES", "FIRES YTD", s_fireCount);

    int cy = CONTENT_Y;

    if (s_fireCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, cy + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawCentreString(!WiFi.isConnected() ? "OFFLINE" : "No wildfires tracked this year",
                                SCREEN_W / 2, cy + 52, FONT_SMALL);
        s_tft->drawCentreString("R=refresh  Q=home", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
        return;
    }

    int limit = min(s_fireCount, (int)(CONTENT_H / ROW_H));
    for (int i = 0; i < limit; i++) {
        int y = cy + i * ROW_H;
        FireItem &fi = s_fires[i];

        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawString("\x2A", 2, y + 2);

        int dw = s_tft->textWidth(fi.when);
        int dateX = SCREEN_W - dw - 2;
        String title = fitTextPx(fi.title, dateX - 16);
        s_tft->setTextColor(COL_WHITE, COL_BG);
        s_tft->drawString(title, 12, y + 2);

        s_tft->setTextColor(COL_CYAN, COL_BG);
        s_tft->drawString(fi.when, dateX, y + 2);

        if (i < limit - 1)
            s_tft->drawFastHLine(0, y + ROW_H - 1, SCREEN_W, COL_GREY_DIM);
    }

    int by = SCREEN_H - BOTTOMBAR_H;
    s_tft->fillRect(0, by, SCREEN_W, BOTTOMBAR_H, COL_BG);
    s_tft->drawFastHLine(0, by, SCREEN_W, COL_CYAN);
    s_tft->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, COL_CYAN);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_CYAN, COL_BG);
    s_tft->drawCentreString("R=refresh  Q=home", SCREEN_W / 2, by + 3, FONT_SMALL);
}

// ── Public API ────────────────────────────────────────────────────────────────
void worldInit(TFT_eSPI &tft) {
    s_tft          = &tft;
    s_showingFires = false;
    s_quakeCount   = 0;
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | USGS", "", COL_CYAN);
    if (WiFi.isConnected()) fetchQuakes();
    updateSyncStr();
    drawQuakesScreen();
}

void worldInitFires(TFT_eSPI &tft) {
    s_tft          = &tft;
    s_showingFires = true;
    s_fireCount    = 0;
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | FIRES", "", COL_CYAN);
    if (WiFi.isConnected()) fetchFires();
    updateSyncStr();
    drawFiresScreen();
}

bool worldLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        if (WiFi.isConnected()) {
            if (s_showingFires) fetchFires();
            else                fetchQuakes();
        }
        updateSyncStr();
        if (s_showingFires) drawFiresScreen();
        else                drawQuakesScreen();
    }

    delay(20);
    return true;
}
