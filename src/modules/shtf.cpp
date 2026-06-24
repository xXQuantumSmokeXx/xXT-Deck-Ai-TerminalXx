#include "shtf.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TinyGPS++.h>
#include <time.h>

// ── Constants ──────────────────────────────────────────────────────────────────
#define KB_ADDR           0x55
#define GPS_RX_PIN        44
#define GPS_TX_PIN        43
#define GPS_BAUD          9600
#define GPS_TIMEOUT_MS    60000

#define NWS_SLOTS         3    // max NWS alerts to fetch
#define FEMA_SLOTS        3    // max FEMA declarations to fetch
#define DISASTER_MAX      (NWS_SLOTS + FEMA_SLOTS)
#define DISASTER_DISPLAY  4    // rows shown (NWS-first, then FEMA)
#define BIO_MAX           9
#define BIO_VISIBLE       7
#define SHTF_CACHE_TTL    3600  // 1 hour

// ── State ──────────────────────────────────────────────────────────────────────
struct DisasterItem {
    char    src[6];    // "NWS" or "FEMA"
    char    event[36]; // "Tornado Warning" / "Hurricane"
    char    detail[14]; // severity "EXT"/"SVR"/"MOD" or state "TX" or "Earth-Quake"
    bool    critical;  // true = red dot, false = amber
};
struct BioItem {
    char    src[10];
    char    text[52];
    uint8_t level;     // 1=amber, 2=red
};

static TFT_eSPI    *s_tft             = nullptr;
static float        s_lat             = 0.0f;
static float        s_lon             = 0.0f;
static bool         s_gpsValid        = false;
static bool         s_gpsCached       = false;
static char         s_fips[8]         = "";
static char         s_countyName[32]  = "";
static char         s_stateAbbr[4]    = "";
static DisasterItem s_disasters[DISASTER_MAX];
static int          s_disasterCount   = 0;
static BioItem      s_bio[BIO_MAX];
static int          s_bioCount        = 0;
static int          s_bioScroll       = 0;
static uint8_t      s_gridScore       = 0;
static uint8_t      s_bioScore        = 0;
static uint8_t      s_cmbScore        = 0;
static char         s_statusLabel[16] = "xX-NOMINAL-Xx";
static uint16_t     s_statusColor     = COL_CYAN;
static char         s_errDisaster[52] = "";
static char         s_errBio[52]      = "";
static uint32_t     s_blinkMs         = 0;
static bool         s_blinkOn         = true;
static bool         s_fromCache       = false;
static TinyGPSPlus  s_gps;

// ── Helpers ────────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

static String xmlField(const String &xml, const char *tag) {
    String open  = String("<") + tag + ">";
    String close = String("</") + tag + ">";
    int s = xml.indexOf(open);
    if (s < 0) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    if (e < 0) return "";
    String v = xml.substring(s, e);
    if (v.startsWith("<![CDATA[") && v.endsWith("]]>"))
        v = v.substring(9, v.length() - 3);
    v.trim();
    return v;
}

// ── GPS ────────────────────────────────────────────────────────────────────────
static bool loadCachedGPS() {
    String latStr = nvsGetString("waze_lat");
    String lonStr = nvsGetString("waze_lon");
    if (latStr.isEmpty() || lonStr.isEmpty()) return false;
    float la = latStr.toFloat(), lo = lonStr.toFloat();
    if (la == 0.0f && lo == 0.0f) return false;
    s_lat = la; s_lon = lo;
    s_gpsValid = true; s_gpsCached = true;
    return true;
}

static bool tryHardwareGPS() {
    s_tft->fillRect(0, CONTENT_Y + 30, SCREEN_W, 30, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("Starting GPS...", SCREEN_W / 2, CONTENT_Y + 34, FONT_SMALL);

    Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    delay(100);
    Serial1.println("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02"); delay(100);
    Serial1.println("$PCAS04,5*1C");                            delay(100);
    Serial1.println("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*02"); delay(100);
    Serial1.println("$PCAS11,3*1E");                            delay(100);

    uint32_t start = millis(), lastDraw = 0;
    while (millis() - start < GPS_TIMEOUT_MS) {
        while (Serial1.available()) {
            if (s_gps.encode(Serial1.read()) && s_gps.location.isValid()) {
                s_lat = (float)s_gps.location.lat();
                s_lon = (float)s_gps.location.lng();
                Serial1.end();
                nvsPutString("waze_lat", String(s_lat, 5));
                nvsPutString("waze_lon", String(s_lon, 5));
                s_gpsValid = true; s_gpsCached = false;
                return true;
            }
        }
        uint32_t now = millis();
        if (now - lastDraw >= 500) {
            lastDraw = now;
            int rem = (int)((GPS_TIMEOUT_MS - (now - start)) / 1000);
            s_tft->fillRect(0, CONTENT_Y + 46, SCREEN_W, 12, COL_BG);
            char buf[52];
            snprintf(buf, sizeof(buf), "Acquiring... %ds (any key=skip)", rem);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawCentreString(buf, SCREEN_W / 2, CONTENT_Y + 48, FONT_SMALL);
        }
        if (readKeyboard() != 0) break;
        delay(50);
    }
    Serial1.end();
    return false;
}

// ── FIPS + county/state lookup ─────────────────────────────────────────────────
static bool fetchFIPS() {
    if (!s_gpsValid) return false;

    String cachedFips   = nvsGetString("shtf_fips");
    String cachedCounty = nvsGetString("shtf_county");
    String cachedState  = nvsGetString("shtf_state");
    if (cachedFips.length() == 5 && cachedCounty.length() > 0 && cachedState.length() > 0) {
        strlcpy(s_fips,       cachedFips.c_str(),   sizeof(s_fips));
        strlcpy(s_countyName, cachedCounty.c_str(), sizeof(s_countyName));
        strlcpy(s_stateAbbr,  cachedState.c_str(),  sizeof(s_stateAbbr));
        return true;
    }

    char url[240];
    snprintf(url, sizeof(url),
        "https://geocoding.geo.census.gov/geocoder/geographies/coordinates"
        "?x=%.5f&y=%.5f&benchmark=4&vintage=4&format=json",
        s_lon, s_lat);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, *http.getStreamPtr())) { http.end(); return false; }
    http.end();

    const char *geoid  = doc["result"]["geographies"]["Counties"][0]["GEOID"]   | "";
    const char *base   = doc["result"]["geographies"]["Counties"][0]["BASENAME"] | "";
    const char *stusab = doc["result"]["geographies"]["States"][0]["STUSAB"]     | "";

    if (strlen(geoid) != 5) return false;

    strlcpy(s_fips,       geoid,  sizeof(s_fips));
    strlcpy(s_countyName, base,   sizeof(s_countyName));
    strlcpy(s_stateAbbr,  stusab, sizeof(s_stateAbbr));

    nvsPutString("shtf_fips",   String(geoid));
    nvsPutString("shtf_county", String(base));
    nvsPutString("shtf_state",  String(stusab));
    return true;
}

// ── Disaster helpers ───────────────────────────────────────────────────────────
static bool isGridEvent(const char *event) {
    static const char *k[] = {
        "Hurricane", "Severe Storm", "Severe Ice Storm", "Tornado",
        "Earthquake", "Winter Storm", "Typhoon", "Coastal Storm",
        "Ice Storm", "Blizzard"
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (strstr(event, k[i])) return true;
    return false;
}

static uint8_t nwsSeverityRank(const char *sev) {
    if (strcmp(sev, "Extreme")  == 0) return 4;
    if (strcmp(sev, "Severe")   == 0) return 3;
    if (strcmp(sev, "Moderate") == 0) return 2;
    if (strcmp(sev, "Minor")    == 0) return 1;
    return 0;
}

// ── NWS active alerts — free, no key, real-time ────────────────────────────────
static void fetchNWS() {
    if (strlen(s_stateAbbr) == 0) return;

    char url[160];
    snprintf(url, sizeof(url),
        "https://api.weather.gov/alerts/active"
        "?area=%s&status=actual&message_type=alert&severity=Extreme%%2CSevere%%2CModerate",
        s_stateAbbr);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Accept", "application/geo+json");
    http.addHeader("User-Agent", "T-Deck-SHTF/1.0");
    http.setTimeout(12000);
    int code = http.GET();
    if (code != 200) { http.end(); return; }

    JsonDocument filter;
    filter["features"][0]["properties"]["event"]    = true;
    filter["features"][0]["properties"]["severity"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, *http.getStreamPtr(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return;

    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull()) return;

    int added = 0;
    for (JsonObject feat : features) {
        if (added >= NWS_SLOTS) break;
        const char *event = feat["properties"]["event"]    | "";
        const char *sev   = feat["properties"]["severity"] | "";
        if (strlen(event) < 2 || nwsSeverityRank(sev) < 2) continue;

        DisasterItem &item = s_disasters[s_disasterCount];
        strlcpy(item.src,   "NWS",  sizeof(item.src));
        strlcpy(item.event, event,  sizeof(item.event));
        if      (strcmp(sev, "Extreme")  == 0) strlcpy(item.detail, "EXT", sizeof(item.detail));
        else if (strcmp(sev, "Severe")   == 0) strlcpy(item.detail, "SVR", sizeof(item.detail));
        else                                   strlcpy(item.detail, "MOD", sizeof(item.detail));
        item.critical = (strcmp(sev, "Extreme") == 0 || strcmp(sev, "Severe") == 0);
        s_disasterCount++;
        added++;
    }
}

// ── USGS significant earthquakes — fallback when FEMA unavailable ──────────────
static void fetchUSGS() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client,
        "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/significant_week.geojson");
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return; }

    JsonDocument filter;
    filter["features"][0]["properties"]["place"] = true;
    filter["features"][0]["properties"]["mag"]   = true;
    filter["features"][0]["properties"]["alert"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, *http.getStreamPtr(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return;

    JsonArray features = doc["features"].as<JsonArray>();
    if (features.isNull()) return;

    int added = 0;
    for (JsonObject feat : features) {
        if (s_disasterCount >= DISASTER_MAX || added >= FEMA_SLOTS) break;
        float mag        = feat["properties"]["mag"]   | 0.0f;
        const char *place = feat["properties"]["place"] | "";
        const char *alert = feat["properties"]["alert"] | "";
        if (mag < 5.0f) continue;

        DisasterItem &item = s_disasters[s_disasterCount];
        strlcpy(item.src, "USGS", sizeof(item.src));
        snprintf(item.event, sizeof(item.event), "M%.1f %s", mag, place);
        strlcpy(item.detail, "(QUAKE)", sizeof(item.detail));
        item.critical = (mag >= 6.0f || strcmp(alert, "red") == 0 || strcmp(alert, "orange") == 0);
        s_disasterCount++;
        added++;
    }
}

// ── FEMA declared disasters — free, no key, most recent national ───────────────
// Uses minimal URL (no OData $filter) to avoid 404 from complex query strings.
// Falls back to USGS significant earthquakes on 404.
static void fetchFEMA() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client,
        "https://www.fema.gov/api/open/v2/disasterDeclarationsSummaries"
        "?$top=10"
        "&$orderby=declarationDate%20desc"
        "&$select=disasterNumber,incidentType,state,declarationDate");
    http.addHeader("Accept", "application/json");
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) {
        fetchUSGS();   // USGS significant earthquakes as fallback
        http.end();
        return;
    }

    JsonDocument filter;
    filter["DisasterDeclarationsSummaries"][0]["disasterNumber"]  = true;
    filter["DisasterDeclarationsSummaries"][0]["incidentType"]    = true;
    filter["DisasterDeclarationsSummaries"][0]["state"]           = true;
    filter["DisasterDeclarationsSummaries"][0]["declarationDate"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, *http.getStreamPtr(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return;

    JsonArray arr = doc["DisasterDeclarationsSummaries"].as<JsonArray>();
    if (arr.isNull()) return;

    // Deduplicate by disaster number
    uint32_t seen[FEMA_SLOTS] = {};
    int seenCount = 0;

    for (JsonObject decl : arr) {
        if (s_disasterCount >= DISASTER_MAX) break;
        if (s_disasterCount - NWS_SLOTS >= FEMA_SLOTS) break;

        uint32_t dnum = (uint32_t)(decl["disasterNumber"] | 0);
        bool dup = false;
        for (int i = 0; i < seenCount; i++) if (seen[i] == dnum) { dup = true; break; }
        if (dup) continue;
        if (seenCount < FEMA_SLOTS) seen[seenCount++] = dnum;

        const char *type  = decl["incidentType"]    | "";
        const char *state = decl["state"]            | "";
        const char *date  = decl["declarationDate"]  | "";

        DisasterItem &item = s_disasters[s_disasterCount];
        strlcpy(item.src,   "FEMA", sizeof(item.src));
        strlcpy(item.event, type,   sizeof(item.event));
        // detail: state abbreviation (2 chars) — shows where since filter is national
        strlcpy(item.detail, state, sizeof(item.detail));
        item.critical = isGridEvent(type);
        s_disasterCount++;
    }
}

// ── Bio fetch helpers ──────────────────────────────────────────────────────────
static void parseRSSItems(const String &xml, const char *src, uint8_t level) {
    int pos = xml.indexOf("<item");
    if (pos < 0) return;
    while (s_bioCount < BIO_MAX) {
        int s = xml.indexOf("<item", pos);
        if (s < 0) break;
        int e = xml.indexOf("</item>", s);
        if (e < 0) break;
        String block = xml.substring(s, min(e + 7, (int)xml.length()));
        String title = xmlField(block, "title");
        title.trim();
        if (title.length() > 3) {
            BioItem &item = s_bio[s_bioCount];
            strlcpy(item.src, src, sizeof(item.src));
            if (title.length() > 51) title = title.substring(0, 49) + "..";
            strlcpy(item.text, title.c_str(), sizeof(item.text));
            item.level = level;
            s_bioCount++;
        }
        pos = e + 7;
    }
}

static void fetchRSS(const char *url, const char *src, uint8_t level) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    if (http.GET() != 200) { http.end(); return; }
    String xml = http.getString();
    http.end();
    if (xml.length() > 8000) xml = xml.substring(0, 8000);
    parseRSSItems(xml, src, level);
}

static void fetchHealthMap() {
    String key = nvsGetString("hmap_key");
    if (key.isEmpty()) return;
    char url[180];
    snprintf(url, sizeof(url),
        "https://healthmap.org/api/?lat=%.4f&lng=%.4f&radius=100&auth=%s",
        s_lat, s_lon, key.c_str());
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    if (http.GET() != 200) { http.end(); return; }
    String json = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    JsonArray arr = doc["data"].as<JsonArray>();
    if (arr.isNull()) arr = doc.as<JsonArray>();
    for (JsonObject alert : arr) {
        if (s_bioCount >= BIO_MAX) break;
        const char *summary = alert["summary"] | alert["description"] | alert["title"] | "";
        if (strlen(summary) < 3) continue;
        BioItem &item = s_bio[s_bioCount];
        strlcpy(item.src, "HlthMap", sizeof(item.src));
        strlcpy(item.text, summary, sizeof(item.text));
        item.level = 2;
        s_bioCount++;
    }
}

// ── Threat score ───────────────────────────────────────────────────────────────
static void computeThreat() {
    // GRID: 25pts per critical NWS alert or grid-type FEMA declaration, cap 100
    int gridEvents = 0;
    for (int i = 0; i < s_disasterCount; i++) {
        const DisasterItem &d = s_disasters[i];
        if (d.critical) gridEvents++;
    }
    s_gridScore = (uint8_t)min(100, gridEvents * 25);

    s_bioScore = (uint8_t)min(100, s_bioCount * 12);
    s_cmbScore = (s_gridScore + s_bioScore) / 2;

    if (s_cmbScore > 65) {
        strlcpy(s_statusLabel, "xX-CRITICAL-Xx", sizeof(s_statusLabel));
        s_statusColor = COL_RED;
    } else if (s_cmbScore > 29) {
        strlcpy(s_statusLabel, "xX-ELEVATED-Xx", sizeof(s_statusLabel));
        s_statusColor = COL_AMBER;
    } else {
        strlcpy(s_statusLabel, "xX-NOMINAL-Xx",  sizeof(s_statusLabel));
        s_statusColor = g_themeColor;
    }
}

// ── Screen draw ────────────────────────────────────────────────────────────────
static void drawThreatBar(int y, const char *label, uint8_t score) {
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(label, 4, y);

    int barX = 4 + s_tft->textWidth(label) + 6;
    int barW = SCREEN_W - barX - 26, barH = 8;
    int fill = (int)(score * (barW - 2) / 100);
    s_tft->fillRect(barX, y, barW, barH, COL_BG);
    s_tft->drawRect(barX, y, barW, barH, g_themeColor);
    if (fill > 0) s_tft->fillRect(barX + 1, y + 1, fill, barH - 2, g_themeColor);

    char sc[5]; snprintf(sc, sizeof(sc), "%d", score);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(sc, barX + barW + 4, y);
}

static void drawShtfScreen() {
    s_tft->fillScreen(COL_BG);

    // Topbar
    char timeBuf[12] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12; else if (h == 0) h = 12;
        snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", h, ti.tm_min, ap);
    }
    const char *cursor = s_blinkOn ? "_" : " ";
    char title[20]; snprintf(title, sizeof(title), ">> SHTF%s", cursor);
    drawTopbar(*s_tft, title, timeBuf, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(s_statusColor, COL_BG);
    s_tft->drawString(s_statusLabel, 80, 4);

    // Status bar — GPS + county
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    if (s_gpsValid) {
        char gpsBuf[52];
        if (strlen(s_countyName) > 0)
            snprintf(gpsBuf, sizeof(gpsBuf), "%s, %s  %.3f,%.3f%s",
                     s_countyName, s_stateAbbr, s_lat, s_lon, s_gpsCached ? "(c)" : "");
        else
            snprintf(gpsBuf, sizeof(gpsBuf), "%.4f, %.4f%s",
                     s_lat, s_lon, s_gpsCached ? " (cached)" : "");
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(gpsBuf, 4, TOPBAR_H + 3);
    } else {
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawString("NO LOCATION  press L or G", 4, TOPBAR_H + 3);
    }
    if (s_fromCache) {
        const char *cTag = "~CACHE";
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawString(cTag, SCREEN_W - 4 - s_tft->textWidth(cTag), TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int y = CONTENT_Y + 2;  // y=36

    // ── DISASTERS (NWS real-time + FEMA declared) ───────────────────────────────
    // header 10px + up to DISASTER_DISPLAY rows at 11px + divider 2px = 45px max
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("[ DISASTERS ]", 4, y);
    y += 10;

    if (s_disasterCount == 0) {
        s_tft->setTextColor(g_themeColor, COL_BG);
        if (strlen(s_errDisaster) > 0)
            s_tft->drawString(s_errDisaster, 4, y);
        else
            s_tft->drawString("[ NO ACTIVE ALERTS ]", 4, y);
        y += 11;
    } else {
        int showN = min(s_disasterCount, DISASTER_DISPLAY);
        for (int i = 0; i < showN; i++) {
            const DisasterItem &d = s_disasters[i];
            uint16_t dotCol = d.critical ? COL_RED : COL_AMBER;
            s_tft->fillCircle(8, y + 4, 2, dotCol);

            // Source label (NWS/FEMA)
            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(dotCol, COL_BG);
            s_tft->drawString(d.src, 14, y);
            int srcEnd = 14 + s_tft->textWidth(d.src) + 2;

            // Detail right-aligned (severity or date)
            s_tft->setTextColor(g_themeColor, COL_BG);
            int detailX = SCREEN_W - s_tft->textWidth(d.detail) - 4;
            s_tft->drawString(d.detail, detailX, y);

            // Event name fills middle, truncated to fit
            String ev = String(d.event);
            while (ev.length() > 3 && srcEnd + s_tft->textWidth(ev) > detailX)
                ev.remove(ev.length() - 1);
            if (ev.length() < strlen(d.event))
                ev = ev.substring(0, ev.length() - 2) + "..";
            s_tft->setTextColor(COL_WHITE, COL_BG);
            s_tft->drawString(ev, srcEnd, y);
            y += 11;
        }
    }
    s_tft->drawFastHLine(4, y, SCREEN_W - 8, g_themeColor); y += 2;

    // ── OUTBREAK (CDC + ProMED + HealthMap) ─────────────────────────────────────
    // header 10px + up to BIO_MAX rows at 11px + divider 2px = 111px max
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("[ OUTBREAK ]", 4, y);
    y += 10;

    if (strlen(s_errBio) > 0) {
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(s_errBio, 4, y);
        y += 11;
    } else if (s_bioCount == 0) {
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString("[ NO DATA ]", 4, y);
        y += 11;
    } else {
        for (int i = 0; i < s_bioCount && i < BIO_VISIBLE; i++) {
            const BioItem &item = s_bio[i];
            uint16_t dotCol = (item.level == 2) ? COL_RED : COL_AMBER;
            s_tft->fillCircle(8, y + 4, 2, dotCol);
            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(dotCol, COL_BG);
            s_tft->drawString(item.src, 14, y);
            int sx = 14 + s_tft->textWidth(item.src) + 4;
            String txt = item.text;
            int maxPx = SCREEN_W - sx - 4;
            while (txt.length() > 3 && s_tft->textWidth(txt) > maxPx)
                txt.remove(txt.length() - 1);
            if (txt.length() < strlen(item.text))
                txt = txt.substring(0, txt.length() - 2) + "..";
            s_tft->setTextColor(COL_WHITE, COL_BG);
            s_tft->drawString(txt, sx, y);
            y += 11;
        }
    }
    s_tft->drawFastHLine(4, y, SCREEN_W - 8, g_themeColor); y += 2;

    // ── THREAT INDEX ────────────────────────────────────────────────────────────
    // header 9px + 3 bars at 9px = 36px
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("[ THREAT INDEX ]", 4, y);
    y += 9;

    drawThreatBar(y, "DISASTERS",  s_gridScore); y += 9;
    drawThreatBar(y, "BIOLOGICAL", s_bioScore);  y += 9;
    drawThreatBar(y, "COMBINED",   s_cmbScore);

    // Themed bottom bar — accent tracks threat level (red/amber/nominal)
    static const BottomKey SHTF_MENU[] = {
        {'R', "FRESH"}, {'L', "LOC"}, {'G', "GPS"}, {'Q', "HOME"}
    };
    drawMenuBar(*s_tft, SHTF_MENU, 4);
}

static void showShtfLoading(const char *msg = "Fetching data...") {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, ">> SHTF", "", g_themeColor);
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("SHTF MONITOR", SCREEN_W / 2, CONTENT_Y + 14, FONT_MED);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString(msg, SCREEN_W / 2, CONTENT_Y + 38, FONT_SMALL);
}

static void saveToCache();  // forward declaration — defined below

static void fetchAll() {
    if (!WiFi.isConnected() || !s_gpsValid) return;
    if (strlen(s_fips) == 0) fetchFIPS();
    // NWS first (real-time), then FEMA fills remaining slots
    s_disasterCount = 0;
    strlcpy(s_errDisaster, "", sizeof(s_errDisaster));
    fetchNWS();
    fetchFEMA();
    // Bio sources
    s_bioCount = 0;
    fetchRSS("https://tools.cdc.gov/api/v2/resources/media/404952.rss", "CDC", 1);
    fetchRSS("https://promedmail.org/feed/", "ProMED", 1);
    fetchHealthMap();
    computeThreat();
    s_fromCache = false;
    saveToCache();
}

// ── Cache ─────────────────────────────────────────────────────────────────────
static void saveToCache() {
    // Pack disasters: src\x01event\x01detail\x01crit\x02...
    String dis;
    for (int i = 0; i < s_disasterCount; i++) {
        const DisasterItem &d = s_disasters[i];
        if (i > 0) dis += '\x02';
        dis += d.src;   dis += '\x01';
        dis += d.event; dis += '\x01';
        dis += d.detail;dis += '\x01';
        dis += (d.critical ? '1' : '0');
    }
    // Pack bio: src\x01text\x01level\x02...
    String bio;
    for (int i = 0; i < s_bioCount; i++) {
        const BioItem &b = s_bio[i];
        if (i > 0) bio += '\x02';
        bio += b.src;  bio += '\x01';
        bio += b.text; bio += '\x01';
        bio += (char)('0' + b.level);
    }
    nvsPutString("shtf_dis",      dis);
    nvsPutString("shtf_bio",      bio);
    nvsPutInt("shtf_dis_n",       s_disasterCount);
    nvsPutInt("shtf_bio_n",       s_bioCount);
    nvsPutInt("shtf_gridscore",   s_gridScore);
    nvsPutInt("shtf_bioscore",    s_bioScore);
    nvsPutInt("shtf_cmbscore",    s_cmbScore);
    nvsPutString("shtf_status",   String(s_statusLabel));
    nvsPutInt("shtf_statclr",     (int)s_statusColor);
    nvsPutInt("shtf_fetch_at",    (int)time(nullptr));
}

static bool loadFromCache() {
    int32_t ts = nvsGetInt("shtf_fetch_at");
    if (ts == 0) return false;
    if ((int32_t)time(nullptr) - ts > SHTF_CACHE_TTL) return false;

    int dCount = nvsGetInt("shtf_dis_n");
    int bCount = nvsGetInt("shtf_bio_n");
    if (dCount < 0 || dCount > DISASTER_MAX) dCount = 0;
    if (bCount < 0 || bCount > BIO_MAX)      bCount = 0;

    // Unpack disasters
    s_disasterCount = 0;
    String dis = nvsGetString("shtf_dis");
    if (dis.length() > 0 && dCount > 0) {
        int pos = 0;
        for (int i = 0; i < dCount && pos < (int)dis.length(); i++) {
            int end = dis.indexOf('\x02', pos);
            if (end < 0) end = dis.length();
            String chunk = dis.substring(pos, end);
            int f1 = chunk.indexOf('\x01');
            int f2 = (f1 >= 0) ? chunk.indexOf('\x01', f1 + 1) : -1;
            int f3 = (f2 >= 0) ? chunk.indexOf('\x01', f2 + 1) : -1;
            if (f3 >= 0) {
                DisasterItem &d = s_disasters[s_disasterCount++];
                strlcpy(d.src,    chunk.substring(0, f1).c_str(),    sizeof(d.src));
                strlcpy(d.event,  chunk.substring(f1+1, f2).c_str(), sizeof(d.event));
                strlcpy(d.detail, chunk.substring(f2+1, f3).c_str(), sizeof(d.detail));
                d.critical = (chunk[f3 + 1] == '1');
            }
            pos = end + 1;
        }
    }

    // Unpack bio
    s_bioCount = 0;
    String bio = nvsGetString("shtf_bio");
    if (bio.length() > 0 && bCount > 0) {
        int pos = 0;
        for (int i = 0; i < bCount && pos < (int)bio.length(); i++) {
            int end = bio.indexOf('\x02', pos);
            if (end < 0) end = bio.length();
            String chunk = bio.substring(pos, end);
            int f1 = chunk.indexOf('\x01');
            int f2 = (f1 >= 0) ? chunk.indexOf('\x01', f1 + 1) : -1;
            if (f2 >= 0) {
                BioItem &b = s_bio[s_bioCount++];
                strlcpy(b.src,  chunk.substring(0, f1).c_str(),    sizeof(b.src));
                strlcpy(b.text, chunk.substring(f1+1, f2).c_str(), sizeof(b.text));
                b.level = (uint8_t)(chunk[f2 + 1] - '0');
                if (b.level < 1 || b.level > 2) b.level = 1;
            }
            pos = end + 1;
        }
    }

    s_gridScore  = (uint8_t)nvsGetInt("shtf_gridscore");
    s_bioScore   = (uint8_t)nvsGetInt("shtf_bioscore");
    s_cmbScore   = (uint8_t)nvsGetInt("shtf_cmbscore");
    String stat  = nvsGetString("shtf_status");
    if (stat.length() > 0) strlcpy(s_statusLabel, stat.c_str(), sizeof(s_statusLabel));
    int clr = nvsGetInt("shtf_statclr");
    if (clr != 0) s_statusColor = (uint16_t)clr;

    s_fromCache = true;
    return true;
}

// ── Boot-time cache warming ──────────────────────────────────────────────────
// Called once at startup before the user navigates to any screen.
// Populates NVS cache so shtfInit() always finds data ready.
void shtfWarmCache() {
    if (!WiFi.isConnected()) return;
    loadCachedGPS();         // reads GPS coords from NVS
    if (!s_gpsValid) return; // need GPS for SHTF data
    // Restore location from NVS (same as shtfInit does)
    String cf = nvsGetString("shtf_fips");
    String cc = nvsGetString("shtf_county");
    String cs = nvsGetString("shtf_state");
    if (cf.length() == 5) strlcpy(s_fips,      cf.c_str(), sizeof(s_fips));
    if (cc.length() > 0)  strlcpy(s_countyName, cc.c_str(), sizeof(s_countyName));
    if (cs.length() > 0)  strlcpy(s_stateAbbr,  cs.c_str(), sizeof(s_stateAbbr));
    fetchAll();              // fetches from NWS/FEMA/HealthMap, saves to NVS
}

// ── Public API ─────────────────────────────────────────────────────────────────
void shtfInit(TFT_eSPI &tft) {
    s_tft          = &tft;
    s_disasterCount = 0;
    s_bioCount     = 0;
    s_bioScroll    = 0;
    s_gpsValid     = false;
    s_gpsCached    = false;
    s_fromCache    = false;
    s_gridScore    = 0;
    s_bioScore     = 0;
    s_cmbScore     = 0;
    strlcpy(s_fips,         "",          sizeof(s_fips));
    strlcpy(s_countyName,   "",          sizeof(s_countyName));
    strlcpy(s_stateAbbr,    "",          sizeof(s_stateAbbr));
    strlcpy(s_errDisaster,  "",          sizeof(s_errDisaster));
    strlcpy(s_errBio,       "",          sizeof(s_errBio));
    strlcpy(s_statusLabel,  "xX-NOMINAL-Xx", sizeof(s_statusLabel));
    s_statusColor = g_themeColor;
    s_blinkMs     = millis();
    s_blinkOn     = true;

    // Restore GPS + location from NVS (fast, no network needed)
    loadCachedGPS();
    String cf = nvsGetString("shtf_fips");
    String cc = nvsGetString("shtf_county");
    String cs = nvsGetString("shtf_state");
    if (cf.length() == 5) strlcpy(s_fips,      cf.c_str(), sizeof(s_fips));
    if (cc.length() > 0)  strlcpy(s_countyName, cc.c_str(), sizeof(s_countyName));
    if (cs.length() > 0)  strlcpy(s_stateAbbr,  cs.c_str(), sizeof(s_stateAbbr));

    // Use cache if still fresh — draw immediately, no loading screen
    if (loadFromCache()) {
        drawShtfScreen();
        return;
    }

    showShtfLoading("Fetching data...");
    fetchAll();
    drawShtfScreen();
}

bool shtfLoop(TFT_eSPI &tft) {
    (void)tft;

    if (millis() - s_blinkMs >= 500) {
        s_blinkMs = millis();
        s_blinkOn = !s_blinkOn;
        const char *cursor = s_blinkOn ? "_" : " ";
        char title[20]; snprintf(title, sizeof(title), ">> SHTF%s", cursor);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->fillRect(4, 4, 74, 12, COL_BG);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(title, 4, 4);
    }

    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("shtf_fetch_at", 0);
        s_fromCache = false;
        showShtfLoading();
        fetchAll();
        drawShtfScreen();
    }

    if (key == 'l' || key == 'L') {
        s_tft->fillScreen(COL_BG);
        drawTopbar(*s_tft, ">> SHTF  SET LOCATION", "", g_themeColor);
        auto readLine = [&](const char *prompt) -> String {
            String buf;
            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(prompt, 4, CONTENT_Y + 8);
            auto redraw = [&]() {
                s_tft->fillRect(0, CONTENT_Y + 22, SCREEN_W, 14, COL_BG);
                s_tft->setTextColor(COL_WHITE, COL_BG);
                s_tft->drawString(buf + "_", 4, CONTENT_Y + 24);
            };
            redraw();
            while (true) {
                char k = readKeyboard();
                if (k == 0) { delay(20); continue; }
                if (k == '\r' || k == '\n') break;
                if ((k == 8 || k == 127) && buf.length() > 0) buf.remove(buf.length() - 1);
                else if (isprint((unsigned char)k) && buf.length() < 32) buf += k;
                redraw();
                delay(20);
            }
            return buf;
        };
        String lat = readLine("Latitude  (e.g. 40.7128):"); lat.trim();
        String lon = readLine("Longitude (e.g. -74.0060):"); lon.trim();
        if (!lat.isEmpty() && !lon.isEmpty()) {
            s_lat = lat.toFloat(); s_lon = lon.toFloat();
            s_gpsValid = true; s_gpsCached = false;
            nvsPutString("waze_lat", lat);
            nvsPutString("waze_lon", lon);
            strlcpy(s_fips, "", sizeof(s_fips));
            strlcpy(s_countyName, "", sizeof(s_countyName));
            strlcpy(s_stateAbbr, "", sizeof(s_stateAbbr));
            nvsPutString("shtf_fips", "");
            nvsPutString("shtf_county", "");
            nvsPutString("shtf_state", "");
            nvsPutInt("shtf_fetch_at", 0);
            showShtfLoading();
            fetchAll();
        }
        drawShtfScreen();
    }

    if (key == 'g' || key == 'G') {
        strlcpy(s_fips, "", sizeof(s_fips));
        strlcpy(s_countyName, "", sizeof(s_countyName));
        strlcpy(s_stateAbbr, "", sizeof(s_stateAbbr));
        nvsPutString("shtf_fips", "");
        nvsPutString("shtf_county", "");
        nvsPutString("shtf_state", "");
        if (tryHardwareGPS()) {
            nvsPutInt("shtf_fetch_at", 0);
            showShtfLoading();
            fetchAll();
        } else {
            strlcpy(s_errDisaster, "GPS timeout — press L for manual", sizeof(s_errDisaster));
        }
        drawShtfScreen();
    }

    delay(20);
    return true;
}

void shtfTrackballUp() {
    if (s_bioScroll > 0) { s_bioScroll--; drawShtfScreen(); }
}
void shtfTrackballDown() {
    int maxOff = max(0, s_bioCount - BIO_VISIBLE);
    if (s_bioScroll < maxOff) { s_bioScroll++; drawShtfScreen(); }
}
