#include "news.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

// Headlines come from the Quantum-Meteor /news route (~1.1 KB, ten stories,
// Reddit with NPR as the fallback) instead of four direct RSS pulls. Two of
// those four feeds were already dead in October 2026: CDC's rss.rss answers 200
// with an empty body and WHO's news-releases.xml is a 404, so the screen had
// been showing ABC and BBC only.
//
// Layout follows USGS/FIRES' full-width rows, but at reading size: 16 px
// headlines in 19 px rows, ten of ten filling the content area. The old 44px
// two-line rows only showed four.

#define KB_ADDR        0x55
#define NEWS_MAX_ITEMS 10
#define NEWS_ROW_H     19
#define NEWS_VISIBLE   (CONTENT_H / NEWS_ROW_H)
#define NEWS_CACHE_TTL (15 * 60)

// ── State ─────────────────────────────────────────────────────────────────────
struct NewsItem {
    char title[112];
    char when[12];
};

static TFT_eSPI *s_tft       = nullptr;
static NewsItem  s_items[NEWS_MAX_ITEMS];
static int       s_itemCount = 0;
static int       s_scrollOff = 0;
static bool      s_cached    = false;
static time_t    s_stampAt   = 0;      // when these headlines arrived
static char      s_errMsg[52] = "";
static int       s_marqueePx   = 0;    // sideways shift for long headlines
static uint32_t  s_marqueeNext = 0;
static bool      s_marqueeHold = false;

static const BottomKey NEWS_MENU[] = { {'R', "FRESH"}, {'Q', "HOME"} };

// ── Helpers ───────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

static void decodeEntities(char *buf, size_t size) {
    String s = buf;
    s.replace("&amp;",  "&");
    s.replace("&lt;",   "<");
    s.replace("&gt;",   ">");
    s.replace("&quot;", "\"");
    s.replace("&#39;",  "'");
    s.replace("&apos;", "'");
    strlcpy(buf, s.c_str(), size);
}

// ── Row layout + sideways ticker ─────────────────────────────────────────────
// One 19 px row per headline. Headlines wider than the row slide sideways a few
// pixels at a time, hold at the end, then snap back — the alternatives were
// truncating the text or shrinking it, and both lose the story (2026-10-23).
static void drawNewsRows(bool movingOnly) {
    s_tft->setTextFont(FONT_MED);
    int visible = min(s_itemCount - s_scrollOff, NEWS_VISIBLE);
    const int avail = SCREEN_W - 8;

    for (int i = 0; i < visible; i++) {
        const NewsItem &item = s_items[s_scrollOff + i];
        int ry = CONTENT_Y + i * NEWS_ROW_H;
        int over = s_tft->textWidth(item.title) - avail;
        if (movingOnly && over <= 0) continue;

        if (movingOnly) s_tft->fillRect(0, ry, SCREEN_W, NEWS_ROW_H, COL_BG);

        int shift = (over > 0) ? min(s_marqueePx, over) : 0;
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(item.title, 4 - shift, ry + 1);
    }
}

static void marqueeTick() {
    if (!s_tft || s_itemCount == 0) return;
    uint32_t now = millis();
    if (now < s_marqueeNext) return;

    s_tft->setTextFont(FONT_MED);
    int visible = min(s_itemCount - s_scrollOff, NEWS_VISIBLE);
    int maxOver = 0;
    for (int i = 0; i < visible; i++) {
        int over = s_tft->textWidth(s_items[s_scrollOff + i].title) - (SCREEN_W - 8);
        if (over > maxOver) maxOver = over;
    }
    if (maxOver <= 0) { s_marqueeNext = now + 500; return; }

    if (s_marqueeHold) {
        // End of a pass: snap back to the start, then rest before moving again.
        s_marqueePx   = 0;
        s_marqueeHold = false;
        s_marqueeNext = now + 1400;
    } else {
        s_marqueePx += 3;
        if (s_marqueePx >= maxOver) {
            s_marqueePx   = maxOver;
            s_marqueeHold = true;
            s_marqueeNext = now + 1600;   // hold the tail long enough to read
        } else {
            s_marqueeNext = now + 90;
        }
    }
    drawNewsRows(true);
}

static void marqueeReset() {
    s_marqueePx   = 0;
    s_marqueeHold = false;
    s_marqueeNext = millis() + 1200;      // read the beginning before it moves
}

// ── NVS cache ─────────────────────────────────────────────────────────────────
static void saveCache() {
    String packed;
    for (int i = 0; i < s_itemCount; i++) {
        if (i > 0) packed += '\x02';
        packed += s_items[i].title;
        packed += '\x01';
        packed += s_items[i].when;
    }
    nvsPutString("news2_d",  packed);
    nvsPutInt(   "news2_n",  s_itemCount);
    nvsPutInt(   "news2_at", (int32_t)time(nullptr));
}

static bool loadCache() {
    int32_t ts = nvsGetInt("news2_at");
    if (ts == 0) return false;
    int32_t now = (int32_t)time(nullptr);
    if (now > 1000000 && now - ts > NEWS_CACHE_TTL) return false;

    int n = nvsGetInt("news2_n");
    if (n <= 0 || n > NEWS_MAX_ITEMS) return false;

    String packed = nvsGetString("news2_d");
    if (packed.length() == 0) return false;

    s_itemCount = 0;
    int pos = 0;
    for (int i = 0; i < n && pos < (int)packed.length(); i++) {
        int end = packed.indexOf('\x02', pos);
        if (end < 0) end = packed.length();
        String chunk = packed.substring(pos, end);
        int f1 = chunk.indexOf('\x01');
        if (f1 >= 0) {
            NewsItem &item = s_items[s_itemCount++];
            strlcpy(item.title, chunk.substring(0, f1).c_str(), sizeof(item.title));
            strlcpy(item.when,  chunk.substring(f1 + 1).c_str(), sizeof(item.when));
        }
        pos = end + 1;
    }
    s_cached = true;
    s_stampAt = (time_t)ts;
    return (s_itemCount > 0);
}

// ── Fetch ─────────────────────────────────────────────────────────────────────
static bool fetchNews() {
    JsonDocument doc;
    if (!httpJson(QM_BASE "/news", doc, 12000)) {
        strlcpy(s_errMsg, "News feed unreachable", sizeof(s_errMsg));
        return false;
    }

    JsonArray items = doc["items"].as<JsonArray>();
    if (items.isNull() || items.size() == 0) {
        strlcpy(s_errMsg, "No stories in feed", sizeof(s_errMsg));
        return false;
    }

    s_itemCount = 0;
    for (JsonVariant it : items) {
        if (s_itemCount >= NEWS_MAX_ITEMS) break;
        const char *title = it["title"] | "";
        if (!title[0]) continue;

        NewsItem &ni = s_items[s_itemCount++];
        strlcpy(ni.title, title, sizeof(ni.title));
        decodeEntities(ni.title, sizeof(ni.title));
        strlcpy(ni.when, it["when"] | "", sizeof(ni.when));
    }

    if (s_itemCount == 0) {
        strlcpy(s_errMsg, "No stories in feed", sizeof(s_errMsg));
        return false;
    }

    s_cached  = false;
    s_stampAt = time(nullptr);
    return true;
}

// ── Screen draw ───────────────────────────────────────────────────────────────
static void drawNewsScreen() {
    s_tft->fillScreen(COL_BG);

    char timeBuf[12];
    fmtClock12(time(nullptr), timeBuf, sizeof(timeBuf));
    drawTopbar(*s_tft, "< HOME | NEWS", timeBuf, g_themeColor);

    // Status bar: fetch stamp on the left, story count on the right — the same
    // shape USGS, FIRES and WEATHER use.
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);

    char stamp[20];
    if (s_stampAt > 0) fmtStamp(s_cached, s_stampAt, stamp, sizeof(stamp));
    else               strlcpy(stamp, "NO DATA", sizeof(stamp));
    s_tft->setTextColor(s_cached ? COL_AMBER : g_themeColor, COL_BG);
    s_tft->drawString(stamp, 4, TOPBAR_H + 3);

    if (s_itemCount > 0) {
        char cntBuf[28];
        if (s_items[0].when[0])
            snprintf(cntBuf, sizeof(cntBuf), "%d STORIES %s", s_itemCount, s_items[0].when);
        else
            snprintf(cntBuf, sizeof(cntBuf), "%d STORIES", s_itemCount);
        int cw = s_tft->textWidth(cntBuf);
        s_tft->setTextColor(s_cached ? COL_AMBER : g_themeColor, COL_BG);
        s_tft->drawString(cntBuf, SCREEN_W - cw - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int y = CONTENT_Y;

    if (s_itemCount == 0) {
        s_tft->setTextFont(FONT_MED);
        if (strlen(s_errMsg) > 0) {
            s_tft->setTextColor(COL_AMBER, COL_BG);
            s_tft->drawCentreString("ERROR", SCREEN_W / 2, y + 18, FONT_MED);
            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawCentreString(s_errMsg, SCREEN_W / 2, y + 40, FONT_SMALL);
        } else {
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawCentreString("NO NEWS", SCREEN_W / 2, y + 22, FONT_MED);
        }
        drawMenuBar(*s_tft, NEWS_MENU, 2);
        return;
    }

    drawNewsRows(false);

    drawMenuBar(*s_tft, NEWS_MENU, 2);
}

static void showLoadingScreen() {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "< HOME | NEWS", "", g_themeColor);
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("NEWS FEED", SCREEN_W / 2, CONTENT_Y + 14, FONT_MED);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->drawCentreString("Fetching headlines...", SCREEN_W / 2, CONTENT_Y + 36, FONT_SMALL);
}

// ── Boot-time cache warming ──────────────────────────────────────────────────
void newsWarmCache() {
    if (!WiFi.isConnected()) return;
    strlcpy(s_errMsg, "", sizeof(s_errMsg));
    if (fetchNews()) saveCache();
}

// ── Public API ────────────────────────────────────────────────────────────────
void newsInit(TFT_eSPI &tft) {
    s_tft        = &tft;
    s_itemCount  = 0;
    s_scrollOff  = 0;
    s_cached     = false;
    s_stampAt    = 0;
    strlcpy(s_errMsg, "", sizeof(s_errMsg));

    marqueeReset();
    if (loadCache()) {
        drawNewsScreen();
        return;
    }

    showLoadingScreen();

    if (WiFi.isConnected()) {
        if (fetchNews()) saveCache();
    } else {
        strlcpy(s_errMsg, "No WiFi connection", sizeof(s_errMsg));
    }

    drawNewsScreen();
}

bool newsLoop(TFT_eSPI &tft) {
    (void)tft;
    char key = readKeyboard();
    if (key == 0) { marqueeTick(); delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("news2_at", 0);
        s_cached    = false;
        s_stampAt   = 0;
        s_itemCount = 0;
        s_scrollOff = 0;
        strlcpy(s_errMsg, "", sizeof(s_errMsg));
        showLoadingScreen();
        if (WiFi.isConnected()) {
            if (fetchNews()) saveCache();
        } else {
            strlcpy(s_errMsg, "No WiFi connection", sizeof(s_errMsg));
        }
        marqueeReset();
        drawNewsScreen();
    }

    delay(20);
    return true;
}

void newsTrackballUp() {
    if (s_scrollOff > 0) { s_scrollOff--; marqueeReset(); drawNewsScreen(); }
}
void newsTrackballDown() {
    int maxOff = max(0, s_itemCount - NEWS_VISIBLE);
    if (s_scrollOff < maxOff) { s_scrollOff++; marqueeReset(); drawNewsScreen(); }
}
