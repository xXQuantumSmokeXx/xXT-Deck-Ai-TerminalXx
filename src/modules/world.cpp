#include "world.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

// Quakes and fires now come from the Quantum-Meteor Worker. The device used to
// pull 14 KB of USGS GeoJSON and 14 KB of EONET events and parse both; the
// Worker serves 2.1 KB (36 quakes) and 5 KB (a merged, deduped WFIGS list).
// USGS row fields are unchanged; fires gained state, acreage and containment
// from WFIGS, so the row shows those instead of a bare title.

#define KB_ADDR        0x55
#define QUAKE_MAX      36
#define FIRE_MAX       24
#define ROW_H          16

#define QUAKE_TTL_SEC  (15 * 60)
#define FIRE_TTL_SEC   (30 * 60)

// ── Data models ───────────────────────────────────────────────────────────────
struct QuakeItem {
    float mag;
    char  place[72];
    char  when[12];
};

struct FireItem {
    char  name[64];
    char  state[4];
    long  acres;
    int   contained;
    char  when[12];
};

static QuakeItem  s_quakes[QUAKE_MAX];
static int        s_quakeCount   = 0;
static FireItem   s_fires[FIRE_MAX];
static int        s_fireCount    = 0;
static bool       s_showingFires  = false;
static int        s_worldScrollOff = 0;
static time_t     s_quakeFetchedAt = 0;
static time_t     s_fireFetchedAt  = 0;
static TFT_eSPI  *s_tft           = nullptr;

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

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

static bool cacheFresh(time_t fetchedAt, int count, int ttlSec) {
    if (count <= 0 || fetchedAt <= 0) return false;
    time_t now = time(nullptr);
    if (now <= 1000000) return true;
    return (now - fetchedAt) <= ttlSec;
}

static uint16_t magColor(float mag) {
    if (mag >= 7.0f) return COL_RED;
    if (mag >= 6.0f) return COL_AMBER;
    if (mag >= 5.0f) return COL_GOLD;
    return g_themeColor;
}

// Worker sends WFIGS discovery time as epoch milliseconds.
static void msToDay(long long ms, char *out, int outLen) {
    time_t t = (time_t)(ms / 1000LL);
    struct tm lt;
    if (t > 0 && localtime_r(&t, &lt)) snprintf(out, outLen, "%02d-%02d", lt.tm_mon + 1, lt.tm_mday);
    else                               strlcpy(out, "--", (size_t)outLen);
}

static void fmtAcres(long a, char *out, int outLen) {
    if (a >= 100000)     snprintf(out, outLen, "%ldk", a / 1000);
    else if (a >= 1000)  snprintf(out, outLen, "%.1fk", a / 1000.0);
    else                 snprintf(out, outLen, "%ld", a);
}

// ── Quake fetch — /quakes {items:[{m,p,w}]} ─────────────────────────────────
static bool fetchQuakes() {
    s_quakeCount = 0;

    JsonDocument doc;
    if (!httpJson(QM_BASE "/quakes", doc, 12000)) return false;

    JsonArray items = doc["items"].as<JsonArray>();
    if (items.isNull()) return false;

    for (JsonVariant it : items) {
        if (s_quakeCount >= QUAKE_MAX) break;
        const char *place = it["p"] | "";
        if (!place[0]) continue;

        QuakeItem &q = s_quakes[s_quakeCount++];
        q.mag = it["m"] | 0.0f;
        truncStr(place, q.place, (int)sizeof(q.place) - 1);
        strlcpy(q.when, it["w"] | "--", sizeof(q.when));
    }

    if (s_quakeCount > 0) s_quakeFetchedAt = time(nullptr);
    return s_quakeCount > 0;
}

// ── Fire fetch — /fire {items:[{n,s,ac,pc,t}]} ──────────────────────────────
static bool fetchFires() {
    s_fireCount = 0;

    JsonDocument doc;
    if (!httpJson(QM_BASE "/fire", doc, 15000)) return false;

    JsonArray items = doc["items"].as<JsonArray>();
    if (items.isNull()) return false;

    for (JsonVariant it : items) {
        if (s_fireCount >= FIRE_MAX) break;
        const char *name = it["n"] | "";
        if (!name[0]) continue;

        FireItem &fi = s_fires[s_fireCount++];
        truncStr(name, fi.name, (int)sizeof(fi.name) - 1);
        strlcpy(fi.state, it["s"] | "--", sizeof(fi.state));
        fi.acres     = it["ac"] | 0L;
        fi.contained = it["pc"] | 0;
        msToDay(it["t"] | 0LL, fi.when, (int)sizeof(fi.when));
    }

    if (s_fireCount > 0) s_fireFetchedAt = time(nullptr);
    return s_fireCount > 0;
}

// ── Shared screen header ──────────────────────────────────────────────────────
// Top bar: title + clock. Status bar: the standard fetch stamp on the left and
// the row count on the right. The old right-hand "M3.5+: 1-12/36" range read
// like a garbled date and left the left half of the bar empty.
static void drawWorldHeader(const char *title, const char *countLabel, int count) {
    char clockStr[12];
    fmtClock12(time(nullptr), clockStr, sizeof(clockStr));
    drawTopbar(*s_tft, title, clockStr, g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);

    time_t fetched = s_showingFires ? s_fireFetchedAt : s_quakeFetchedAt;
    char stamp[20];
    if (fetched > 0) fmtStamp(false, fetched, stamp, sizeof(stamp));
    else             strlcpy(stamp, "NO DATA", sizeof(stamp));
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(stamp, 4, TOPBAR_H + 3);

    if (count > 0) {
        char buf[28];
        snprintf(buf, sizeof(buf), "%s %d", countLabel, count);
        int w = s_tft->textWidth(buf);
        s_tft->drawString(buf, SCREEN_W - w - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);
}

// ── Quakes screen ─────────────────────────────────────────────────────────────
static void drawQuakesScreen() {
    s_tft->fillScreen(COL_BG);
    drawWorldHeader("< HOME | USGS", "M3.5+", s_quakeCount);

    int cy = CONTENT_Y;

    if (s_quakeCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, cy + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString(!WiFi.isConnected() ? "OFFLINE" : "Worker unreachable",
                                SCREEN_W / 2, cy + 52, FONT_SMALL);
        drawMenuBar(*s_tft, "R=REFRESH  Q=HOME");
        return;
    }

    int visible = (int)(CONTENT_H / ROW_H);
    int limit   = min(s_quakeCount - s_worldScrollOff, visible);
    for (int i = 0; i < limit; i++) {
        int y = cy + i * ROW_H;
        QuakeItem &q = s_quakes[s_worldScrollOff + i];
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
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(place, placeX, y + 2);

        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(q.when, dateX, y + 2);
    }

    drawMenuBar(*s_tft, "R=REFRESH  Q=HOME");
}

// ── Fires screen ──────────────────────────────────────────────────────────────
static void drawFiresScreen() {
    s_tft->fillScreen(COL_BG);
    drawWorldHeader("< HOME | FIRES", "WILDFIRES", s_fireCount);

    int cy = CONTENT_Y;

    if (s_fireCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, cy + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString(!WiFi.isConnected() ? "OFFLINE" : "Worker unreachable",
                                SCREEN_W / 2, cy + 52, FONT_SMALL);
        drawMenuBar(*s_tft, "R=REFRESH  Q=HOME");
        return;
    }

    int visible = (int)(CONTENT_H / ROW_H);
    int limit   = min(s_fireCount - s_worldScrollOff, visible);
    for (int i = 0; i < limit; i++) {
        int y = cy + i * ROW_H;
        FireItem &fi = s_fires[s_worldScrollOff + i];

        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawString("\x2A", 2, y + 2);

        // Right column: state, acreage, containment. The date moved to the
        // title's line only when there is no acreage to show.
        char acres[10], right[24];
        fmtAcres(fi.acres, acres, sizeof(acres));
        if (fi.contained > 0)
            snprintf(right, sizeof(right), "%s %sac %d%%", fi.state, acres, fi.contained);
        else
            snprintf(right, sizeof(right), "%s %sac", fi.state, acres);

        int rw = s_tft->textWidth(right);
        int rightX = SCREEN_W - rw - 2;
        String name = fitTextPx(fi.name, rightX - 16);

        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(name, 12, y + 2);

        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(right, rightX, y + 2);
    }

    drawMenuBar(*s_tft, "R=REFRESH  Q=HOME");
}

// ── Boot-time cache warming ──────────────────────────────────────────────────
// Called once at startup before the user navigates to any screen.
// Populates in-memory cache so worldInit() / worldInitFires() find data ready.
void worldWarmCache() {
    if (!WiFi.isConnected()) return;
    fetchQuakes();
    fetchFires();
}

// ── Public API ────────────────────────────────────────────────────────────────
void worldInit(TFT_eSPI &tft) {
    s_tft           = &tft;
    s_showingFires  = false;
    s_worldScrollOff = 0;
    if (!cacheFresh(s_quakeFetchedAt, s_quakeCount, QUAKE_TTL_SEC)) {
        tft.fillScreen(COL_BG);
        drawTopbar(tft, "< HOME | USGS", "", g_themeColor);
        tft.setTextFont(FONT_SMALL);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.drawCentreString("Fetching USGS data...", SCREEN_W / 2, SCREEN_H / 2, FONT_SMALL);
        if (WiFi.isConnected()) fetchQuakes();
    }
    drawQuakesScreen();
}

void worldInitFires(TFT_eSPI &tft) {
    s_tft           = &tft;
    s_showingFires  = true;
    s_worldScrollOff = 0;
    if (!cacheFresh(s_fireFetchedAt, s_fireCount, FIRE_TTL_SEC)) {
        tft.fillScreen(COL_BG);
        drawTopbar(tft, "< HOME | FIRES", "", g_themeColor);
        tft.setTextFont(FONT_SMALL);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.drawCentreString("Fetching fire data...", SCREEN_W / 2, SCREEN_H / 2, FONT_SMALL);
        if (WiFi.isConnected()) fetchFires();
    }
    drawFiresScreen();
}

bool worldLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        s_worldScrollOff = 0;
        if (WiFi.isConnected()) {
            if (s_showingFires) fetchFires();
            else                fetchQuakes();
        }
        if (s_showingFires) drawFiresScreen();
        else                drawQuakesScreen();
    }

    delay(20);
    return true;
}

void worldTrackballUp() {
    if (s_worldScrollOff > 0) {
        s_worldScrollOff--;
        if (s_showingFires) drawFiresScreen();
        else                drawQuakesScreen();
    }
}

void worldTrackballDown() {
    int visible = (int)(CONTENT_H / ROW_H);
    int total   = s_showingFires ? s_fireCount : s_quakeCount;
    if (s_worldScrollOff < total - visible) {
        s_worldScrollOff++;
        if (s_showingFires) drawFiresScreen();
        else                drawQuakesScreen();
    }
}
