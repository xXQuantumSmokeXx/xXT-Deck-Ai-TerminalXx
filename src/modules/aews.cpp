#include "aews.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <time.h>

// Ported from CYD-Weather's screen_ews.cpp. Same data and hierarchy, rebuilt
// for the T-Deck's chrome: the status bar carries the sync stamp, the bottom
// bar carries the keys, and the screen is drawn once per fetch instead of
// repainted every frame.

#define KB_ADDR        0x55
#define AEWS_TTL_MS    (35UL * 60UL * 1000UL)   // EWS data updates every 30 min
#define AEWS_RETRY_MS  30000UL                  // after a failure, back off

// ── Data ─────────────────────────────────────────────────────────────────────
struct AewsData {
    int   level    = 1;
    float z        = 0.0f;
    int   jets     = 0;
    int   baseline = 0;
    int   tracked  = 0;
    char  alert[16]  = "Normal";
    char  updated[12] = "--:--";
    bool  valid    = false;
};

static AewsData  s_ews;
static TFT_eSPI *s_tft       = nullptr;
static uint32_t  s_fetchedMs = 0;
static uint32_t  s_attemptMs = 0;
static bool      s_force     = false;
static bool      s_lastFail  = false;
static char      s_sync[12]  = "--:--";

// ── Helpers ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

static void stampSync() {
    fmtClock12(time(nullptr), s_sync, sizeof(s_sync));
}

// Green (1) → amber (3) → red (5), same ladder as the CYD screen.
static uint16_t levelColor(int level) {
    switch (level) {
        case 1:  return 0x07E0;    // green
        case 2:  return 0x9FE0;    // yellow-green
        case 3:  return COL_AMBER;
        case 4:  return 0xFC00;    // orange
        default: return COL_RED;
    }
}

static void fmtCount(int n, char *buf, size_t len) {
    if (n >= 1000000)   snprintf(buf, len, "%d,%03d,%03d", n / 1000000, (n / 1000) % 1000, n % 1000);
    else if (n >= 1000) snprintf(buf, len, "%d,%03d", n / 1000, n % 1000);
    else                snprintf(buf, len, "%d", n);
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithm). Kept local so the module does not depend on timegm().
static time_t epochFromUtc(int y, int mo, int d, int h, int mi) {
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400L + h * 3600L + mi * 60L;
}

// "2026-10-23T22:29:50+00:00" → local "6:29 PM"
static void isoToLocalHM(const char *iso, char *out, size_t len) {
    if (!iso || strlen(iso) < 16) { strlcpy(out, "--:--", len); return; }
    int Y = atoi(iso), M = atoi(iso + 5), D = atoi(iso + 8);
    int h = atoi(iso + 11), m = atoi(iso + 14);
    time_t t = epochFromUtc(Y, M, D, h, m);
    struct tm lt;
    localtime_r(&t, &lt);
    int lh = lt.tm_hour;
    const char *ap = lh >= 12 ? "PM" : "AM";
    if (lh > 12) lh -= 12; else if (lh == 0) lh = 12;
    snprintf(out, len, "%d:%02d %s", lh, lt.tm_min, ap);
}

// ── Fetch ───────────────────────────────────────────────────────────────────
static bool fetchAews() {
    s_attemptMs = millis();

    JsonDocument doc;
    if (!httpJson(QM_BASE "/ews", doc)) { s_lastFail = true; return false; }

    int level = doc["level"] | 1;
    if (level < 1 || level > 5) level = 1;

    s_ews.level    = level;
    s_ews.z        = doc["z"]        | 0.0f;
    s_ews.jets     = doc["jets"]     | 0;
    s_ews.baseline = doc["baseline"] | 0;
    s_ews.tracked  = doc["tracked"]  | 0;

    const char *alert = doc["alert"] | "normal";
    strlcpy(s_ews.alert, alert, sizeof(s_ews.alert));
    if (s_ews.alert[0] >= 'a' && s_ews.alert[0] <= 'z') s_ews.alert[0] -= 32;

    isoToLocalHM(doc["updated"] | "", s_ews.updated, sizeof(s_ews.updated));

    s_ews.valid = true;
    s_lastFail  = false;
    s_fetchedMs = millis();
    stampSync();
    return true;
}

static bool needsFetch() {
    if (!s_ews.valid) return s_attemptMs == 0 || (millis() - s_attemptMs > AEWS_RETRY_MS);
    if (s_force) return true;
    return millis() - s_fetchedMs > AEWS_TTL_MS;
}

// ── Draw ────────────────────────────────────────────────────────────────────
static void drawAews() {
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
    drawTopbar(t, "< HOME | AEWS", clockStr, g_themeColor);

    // ── Status bar: what this screen is, and when it last synced ────────────
    t.fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    t.setTextFont(FONT_SMALL);
    t.setTextColor(g_themeColor, COL_BG);
    t.drawString("APOCALYPSE EARLY WARNING", 4, TOPBAR_H + 3);

    char syncBuf[20];
    snprintf(syncBuf, sizeof(syncBuf), "%s %s", s_lastFail ? "FAIL" : "LIVE", s_sync);
    int sw = t.textWidth(syncBuf);
    t.setTextColor(s_lastFail ? COL_AMBER : g_themeColor, COL_BG);
    t.drawString(syncBuf, SCREEN_W - sw - 4, TOPBAR_H + 3);
    t.drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);
    t.fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    if (!s_ews.valid) {
        t.setTextFont(FONT_MED);
        t.setTextColor(g_themeColor, COL_BG);
        t.drawCentreString("NO AEWS DATA", SCREEN_W / 2, CONTENT_Y + 60, FONT_MED);
        t.setTextFont(FONT_SMALL);
        t.drawCentreString(WiFi.isConnected() ? "Worker unreachable - press R"
                                              : "OFFLINE - no connection",
                           SCREEN_W / 2, CONTENT_Y + 84, FONT_SMALL);
        drawMenuBar(t, "Q=HOME  R=REFRESH");
        return;
    }

    const int cx = SCREEN_W / 2;
    const uint16_t lc = levelColor(s_ews.level);
    int y = CONTENT_Y + 12;

    // ── Big level number + "/5" ────────────────────────────────────────────
    char lvl[4];
    snprintf(lvl, sizeof(lvl), "%d", s_ews.level);

    t.setTextFont(FONT_NUM);
    int lw = t.textWidth(lvl);
    t.setTextFont(FONT_LARGE);
    int sw5 = t.textWidth("/5");
    int blockW = lw + 8 + sw5;

    t.setTextFont(FONT_NUM);
    t.setTextColor(lc, COL_BG);
    t.drawString(lvl, cx - blockW / 2, y);

    t.setTextFont(FONT_LARGE);
    t.drawString("/5", cx - blockW / 2 + lw + 8, y + 22);

    // ── Alert word ────────────────────────────────────────────────────────
    y += 52;
    t.setTextFont(FONT_LARGE);
    t.setTextColor(lc, COL_BG);
    int aw = t.textWidth(s_ews.alert);
    t.drawString(s_ews.alert, cx - aw / 2, y);

    // ── Z-score ───────────────────────────────────────────────────────────
    y += 28;
    t.setTextFont(FONT_MED);
    t.setTextColor(COL_WHITE, COL_BG);
    char zbuf[16];
    snprintf(zbuf, sizeof(zbuf), "%+.2f z", (double)s_ews.z);
    int zw = t.textWidth(zbuf);
    t.drawString(zbuf, cx - zw / 2, y);

    // ── Divider ───────────────────────────────────────────────────────────
    y += 20;
    t.drawFastHLine(20, y, SCREEN_W - 40, g_themeColor);

    // ── Stats: two rows, two columns ──────────────────────────────────────
    y += 8;
    const int col1x = 16;
    const int col2x = 170;
    t.setTextFont(FONT_SMALL);

    auto statRow = [&](int x, int rowY, const char *label, const char *value, uint16_t vcol) {
        t.setTextColor(g_themeColor, COL_BG);
        t.drawString(label, x, rowY);
        int lw2 = t.textWidth(label);
        t.setTextColor(vcol, COL_BG);
        t.drawString(value, x + lw2 + 6, rowY);
    };

    char buf[16];
    fmtCount(s_ews.jets, buf, sizeof(buf));
    statRow(col1x, y, "JETS", buf, COL_WHITE);
    fmtCount(s_ews.tracked, buf, sizeof(buf));
    statRow(col2x, y, "TRACKED", buf, COL_WHITE);

    y += 18;
    fmtCount(s_ews.baseline, buf, sizeof(buf));
    statRow(col1x, y, "BASELINE", buf, COL_WHITE);
    int dev = s_ews.jets - s_ews.baseline;
    snprintf(buf, sizeof(buf), "%+d", dev);
    statRow(col2x, y, "DEV", buf, dev > 0 ? COL_AMBER : COL_WHITE);

    // ── Provenance ────────────────────────────────────────────────────────
    y += 24;
    t.setTextFont(FONT_SMALL);
    t.setTextColor(g_themeColor, COL_BG);
    char upd[44];
    snprintf(upd, sizeof(upd), "DATA %s LOCAL  \xB7  ADS-B EXCHANGE", s_ews.updated);
    t.drawString(upd, 16, y);

    drawMenuBar(t, "Q=HOME  R=REFRESH");
}

// ── Public API ──────────────────────────────────────────────────────────────
void aewsInit(TFT_eSPI &tft) {
    s_tft = &tft;
    s_force = false;

    if (!s_ews.valid) {
        drawAews();                              // shows the empty state
        if (WiFi.isConnected() && needsFetch()) {
            fetchAews();
            drawAews();
        }
        return;
    }

    drawAews();
    if (WiFi.isConnected() && needsFetch()) {
        fetchAews();
        drawAews();
    }
}

bool aewsLoop(TFT_eSPI &tft) {
    (void)tft;
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        s_force = true;
        s_attemptMs = 0;
        if (WiFi.isConnected()) fetchAews();
        s_force = false;
        stampSync();
        drawAews();
    }

    delay(20);
    return true;
}

void aewsWarmCache() {
    if (!WiFi.isConnected()) return;
    fetchAews();
}
