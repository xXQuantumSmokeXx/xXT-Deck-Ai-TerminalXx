#include "outages.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <time.h>

// Ported from CYD-Weather's screen_outages.cpp. The /grid route decides the
// "local" state from Cloudflare's view of this device's address (request.cf
// .region), so this screen needs no coordinates and no location setting — the
// CYD code relies on the same behaviour. National history is sampled into the
// Worker's KV every ~10 min and rolls over 24 h, so the chart fills in over
// the first day and the screen must handle a short series.

#define KB_ADDR        0x55
#define OUT_TTL_MS     (10UL * 60UL * 1000UL)   // matches the Worker's cache
#define OUT_RETRY_MS   30000UL
#define OUT_PTS        30

#define X0 6
#define X1 (SCREEN_W - 6)

// ── Data ────────────────────────────────────────────────────────────────────
struct OutSeries {
    long   now   = 0;
    long   ago   = 0;
    long   peak  = 0;
    long   delta = 0;
    int8_t dir   = 0;          // -1 falling, 0 steady, 1 rising
    uint8_t n    = 0;          // points in pts[]
    long   pts[OUT_PTS] = {};
};

struct OutStateRow {
    char abbr[4] = "";
    long out     = 0;
};

static OutSeries   s_nat;
static OutSeries   s_loc;
static bool        s_hasNat      = false;
static bool        s_hasLoc      = false;
static char        s_localName[24] = "";
static long        s_incidents   = 0;
static long        s_counties    = 0;
static OutStateRow s_states[4];
static int         s_stateCount  = 0;
static bool        s_stale       = false;
static bool        s_lastFail    = false;
static char        s_sync[12]    = "--:--";

static TFT_eSPI *s_tft       = nullptr;
static uint32_t  s_fetchedMs = 0;
static uint32_t  s_attemptMs = 0;
static bool      s_force     = false;

// ── Helpers ─────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

static void stampSync() {
    fmtClock12(time(nullptr), s_sync, sizeof(s_sync));
}

static void copyFit(const char *src, char *dst, size_t len) {
    if (!src || len == 0) { if (len) dst[0] = '\0'; return; }
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

static void fmtComma(long n, char *buf, size_t len) {
    if (n < 0) n = 0;
    if (n >= 1000000)   snprintf(buf, len, "%ld,%03ld,%03ld", n / 1000000, (n / 1000) % 1000, n % 1000);
    else if (n >= 1000) snprintf(buf, len, "%ld,%03ld", n / 1000, n % 1000);
    else                snprintf(buf, len, "%ld", n);
}

static void fmtShort(long n, char *buf, size_t len) {
    if (n < 0) n = 0;
    if (n >= 100000)    snprintf(buf, len, "%ldk", n / 1000);
    else if (n >= 1000) snprintf(buf, len, "%.1fk", n / 1000.0);
    else                snprintf(buf, len, "%ld", n);
}

static bool readSeries(JsonVariant v, OutSeries &s) {
    if (v.isNull()) return false;
    s.now   = v["now"]   | 0L;
    s.ago   = v["ago"]   | 0L;
    s.peak  = v["peak"]  | 0L;
    s.delta = v["delta"] | 0L;
    s.dir   = (int8_t)(v["dir"] | 0);
    s.n     = 0;
    for (JsonVariant p : v["series"].as<JsonArray>()) {
        if (s.n >= OUT_PTS) break;
        s.pts[s.n++] = p.as<long>();
    }
    if (s.peak < s.now) s.peak = s.now;
    return true;
}

// ── Fetch ───────────────────────────────────────────────────────────────────
static bool fetchOutages() {
    s_attemptMs = millis();

    JsonDocument doc;
    if (!httpJson(QM_BASE "/grid", doc)) { s_lastFail = true; return false; }

    OutSeries nat, loc;
    if (!readSeries(doc["national"], nat)) { s_lastFail = true; return false; }
    bool hasLoc = readSeries(doc["local"], loc);

    char localName[24] = "";
    if (hasLoc) copyFit(doc["local"]["name"] | "", localName, sizeof(localName));

    OutStateRow rows[4];
    int rowCount = 0;
    for (JsonVariant s : doc["states"].as<JsonArray>()) {
        if (rowCount >= 4) break;
        copyFit(s["abbr"] | "", rows[rowCount].abbr, sizeof(rows[rowCount].abbr));
        rows[rowCount].out = s["out"] | 0L;
        rowCount++;
    }

    s_nat        = nat;
    s_loc        = loc;
    s_hasNat     = true;
    s_hasLoc     = hasLoc;
    copyFit(localName, s_localName, sizeof(s_localName));
    s_incidents  = doc["incidents"] | 0L;
    s_counties   = doc["counties"]  | 0L;
    s_stale      = doc["stale"]     | false;
    s_stateCount = rowCount;
    for (int i = 0; i < rowCount; i++) s_states[i] = rows[i];

    s_lastFail = false;
    s_fetchedMs = millis();
    stampSync();
    return true;
}

static bool needsFetch() {
    if (!s_hasNat) return s_attemptMs == 0 || (millis() - s_attemptMs > OUT_RETRY_MS);
    if (s_force) return true;
    return millis() - s_fetchedMs > OUT_TTL_MS;
}

// ── Draw helpers ────────────────────────────────────────────────────────────
static void drawDelta(TFT_eSPI &t, const OutSeries &s, int y) {
    char mag[12], txt[18];
    long m = s.delta < 0 ? -s.delta : s.delta;
    fmtShort(m, mag, sizeof(mag));

    if (s.dir > 0)      snprintf(txt, sizeof(txt), "+%s", mag);
    else if (s.dir < 0) snprintf(txt, sizeof(txt), "-%s", mag);
    else                snprintf(txt, sizeof(txt), "flat");

    uint16_t col = (s.dir > 0) ? COL_AMBER : (s.dir < 0 ? 0x07E0 : g_themeColor);
    t.setTextFont(FONT_SMALL);
    t.setTextColor(col, COL_BG);
    int tw = t.textWidth(txt);
    int tx = X1 - tw;
    t.drawString(txt, tx, y);

    int cx = tx - 7;
    int cy = y + 4;
    if (s.dir > 0)      t.fillTriangle(cx, cy + 3, cx + 6, cy + 3, cx + 3, cy - 3, col);
    else if (s.dir < 0) t.fillTriangle(cx, cy - 3, cx + 6, cy - 3, cx + 3, cy + 3, col);
}

// One 24 h block: label + delta on top, bars, then now/peak underneath.
static void drawChart(TFT_eSPI &t, const OutSeries &s, const char *label, int y, int barH) {
    t.setTextFont(FONT_SMALL);
    t.setTextColor(g_themeColor, COL_BG);
    t.drawString(label, X0, y);
    drawDelta(t, s, y);

    int barTop = y + 12;
    int barBot = barTop + barH;
    t.drawFastHLine(X0, barBot, SCREEN_W - 2 * X0, g_themeColor);

    if (s.n >= 2) {
        long maxv = s.peak > 0 ? s.peak : 1;
        int areaW = SCREEN_W - 2 * X0;
        int bw = (areaW - (s.n - 1)) / s.n;
        if (bw < 2) bw = 2;

        for (int i = 0; i < s.n; i++) {
            int bh = (int)(((long long)s.pts[i] * barH) / maxv);
            if (bh < 1 && s.pts[i] > 0) bh = 1;
            if (bh == 0) continue;
            int x = X0 + i * (bw + 1);
            uint16_t c = (i == s.n - 1) ? COL_WHITE : g_themeColor;
            t.fillRect(x, barBot - bh, bw, bh, c);
        }
    } else {
        t.setTextFont(FONT_SMALL);
        t.setTextColor(g_themeColor, COL_BG);
        t.drawString("no 24h history yet", X0 + 2, barTop + barH / 2 - 4);
    }

    char nowBuf[12], peakBuf[12], txt[24];
    fmtShort(s.now, nowBuf, sizeof(nowBuf));
    fmtShort(s.peak, peakBuf, sizeof(peakBuf));

    t.setTextFont(FONT_MED);
    t.setTextColor(COL_WHITE, COL_BG);
    snprintf(txt, sizeof(txt), "%s now", nowBuf);
    t.drawString(txt, X0, barBot + 2);

    t.setTextColor(g_themeColor, COL_BG);
    snprintf(txt, sizeof(txt), "peak %s", peakBuf);
    int pw = t.textWidth(txt);
    t.drawString(txt, X1 - pw, barBot + 2);
}

// ── Draw ────────────────────────────────────────────────────────────────────
static void drawOutages() {
    if (!s_tft) return;
    TFT_eSPI &t = *s_tft;

    char clockStr[10] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12; else if (h == 0) h = 12;
        snprintf(clockStr, sizeof(clockStr), "%d:%02d %s", h, ti.tm_min, ap);
    }
    drawTopbar(t, "< HOME | OUTAGES", clockStr, g_themeColor);

    // ── Status bar: the headline number (same slot CYD uses) ───────────────
    t.fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    t.setTextFont(FONT_SMALL);
    t.setTextColor(COL_WHITE, COL_BG);

    char hdr[32], cbuf[16];
    if (s_hasNat) {
        fmtComma(s_nat.now, cbuf, sizeof(cbuf));
        snprintf(hdr, sizeof(hdr), "REPORTED OUT: %s", cbuf);
    } else {
        strlcpy(hdr, "REPORTED OUT: --", sizeof(hdr));
    }
    t.drawString(hdr, 4, TOPBAR_H + 3);

    char syncBuf[20];
    snprintf(syncBuf, sizeof(syncBuf), "%s %s",
             s_stale ? "STALE" : (s_lastFail ? "FAIL" : "LIVE"), s_sync);
    int sw = t.textWidth(syncBuf);
    t.setTextColor((s_stale || s_lastFail) ? COL_AMBER : g_themeColor, COL_BG);
    t.drawString(syncBuf, SCREEN_W - sw - 4, TOPBAR_H + 3);
    t.drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);
    t.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (!s_hasNat) {
        t.setTextFont(FONT_MED);
        t.setTextColor(g_themeColor, COL_BG);
        t.drawCentreString("NO OUTAGE DATA", SCREEN_W / 2, CONTENT_Y + 58, FONT_MED);
        t.setTextFont(FONT_SMALL);
        t.drawCentreString(WiFi.isConnected() ? "Worker unreachable - press R"
                                              : "OFFLINE - no connection",
                           SCREEN_W / 2, CONTENT_Y + 82, FONT_SMALL);
        drawMenuBar(t, "Q=HOME  R=REFRESH");
        return;
    }

    // ── Two 24 h charts ────────────────────────────────────────────────────
    const int chart1Y = CONTENT_Y + 4;    // 38
    const int chart2Y = chart1Y + 66;     // 104

    drawChart(t, s_nat, "NATIONAL 24H", chart1Y, 30);

    if (s_hasLoc) {
        char label[32];
        size_t i = 0;
        for (; s_localName[i] && i < sizeof(label) - 5; i++)
            label[i] = (char)toupper((unsigned char)s_localName[i]);
        label[i] = '\0';
        strncat(label, " 24H", sizeof(label) - strlen(label) - 1);
        drawChart(t, s_loc, label, chart2Y, 30);
    } else {
        t.setTextFont(FONT_SMALL);
        t.setTextColor(g_themeColor, COL_BG);
        t.drawString("LOCAL 24H", X0, chart2Y);
        t.drawString("state unavailable", X0 + 2, chart2Y + 30);
    }

    // ── Totals ─────────────────────────────────────────────────────────────
    int y = chart2Y + 76;                 // 180 — clears the chart's now/peak row
    t.drawFastHLine(0, y - 4, SCREEN_W, g_themeColor);

    char a[16], line[40];
    fmtComma(s_incidents, a, sizeof(a));
    t.setTextFont(FONT_MED);
    t.setTextColor(COL_WHITE, COL_BG);
    snprintf(line, sizeof(line), "%s incidents", a);
    t.drawString(line, X0, y);

    // ODIN covers participating utilities only, so the number is reported,
    // never a national total — keep the source tag on the right.
    t.setTextFont(FONT_SMALL);
    t.setTextColor(g_themeColor, COL_BG);
    if (s_counties > 0) {
        char b[16];
        fmtComma(s_counties, b, sizeof(b));
        snprintf(line, sizeof(line), "%s counties", b);
    } else {
        strlcpy(line, "DOE/ORNL ODIN", sizeof(line));
    }
    int w = t.textWidth(line);
    t.drawString(line, X1 - w, y + 3);

    int x = X0;
    for (int i = 0; i < s_stateCount; i++) {
        char o[12], cell[24];
        fmtShort(s_states[i].out, o, sizeof(o));
        snprintf(cell, sizeof(cell), "%s %s", s_states[i].abbr, o);
        t.drawString(cell, x, y + 20);
        x += t.textWidth(cell) + 12;
    }

    drawMenuBar(t, "Q=HOME  R=REFRESH");
}

// ── Public API ──────────────────────────────────────────────────────────────
void outagesInit(TFT_eSPI &tft) {
    s_tft = &tft;
    s_force = false;

    drawOutages();
    if (WiFi.isConnected() && needsFetch()) {
        fetchOutages();
        drawOutages();
    }
}

bool outagesLoop(TFT_eSPI &tft) {
    (void)tft;
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        s_force = true;
        s_attemptMs = 0;
        if (WiFi.isConnected()) fetchOutages();
        s_force = false;
        drawOutages();
    }

    delay(20);
    return true;
}

void outagesWarmCache() {
    if (!WiFi.isConnected()) return;
    fetchOutages();
}
