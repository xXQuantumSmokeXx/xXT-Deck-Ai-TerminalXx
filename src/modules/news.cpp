#include "news.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include "../net/json_get.h"
#include <Wire.h>
#include <WiFi.h>
#include <time.h>

// Headlines now come from the Quantum-Meteor /news route (~1.1 KB, ten
// stories, Reddit with NPR as the fallback) instead of four direct RSS pulls.
// Two of those four feeds were already dead in October 2026: CDC's rss.rss
// answers 200 with an empty body and WHO's news-releases.xml is a 404, so the
// screen had been showing ABC and BBC only. The trade is deliberate: agency
// health feeds are gone, general news is back.

#define KB_ADDR        0x55
#define NEWS_MAX_ITEMS 10
#define NEWS_VISIBLE   4
#define NEWS_ROW_H     44
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
static char      s_errMsg[52] = "";

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

    s_cached = false;
    return true;
}

// ── Word-wrap helper — splits a headline across two lines ────────────────────
static void wordWrap(const char *text, int maxW1, int maxW2, String &l1, String &l2) {
    String full(text);
    s_tft->setTextFont(FONT_MED);
    if (s_tft->textWidth(full) <= maxW1) { l1 = full; l2 = ""; return; }

    int split = -1;
    for (int i = 0; i < (int)full.length(); i++) {
        if (full[i] != ' ') continue;
        if (s_tft->textWidth(full.substring(0, i)) <= maxW1) split = i;
        else break;
    }
    if (split < 0) {
        l1 = full;
        while (l1.length() > 1 && s_tft->textWidth(l1) > maxW1) l1.remove(l1.length() - 1);
        l2 = "";
        return;
    }
    l1 = full.substring(0, split);
    l2 = full.substring(split + 1);
    if (s_tft->textWidth(l2) > maxW2) {
        while (l2.length() > 2 && (int)l2.length() > 0 && s_tft->textWidth(l2 + "..") > maxW2)
            l2.remove(l2.length() - 1);
        l2 += "..";
    }
}

// ── Screen draw ───────────────────────────────────────────────────────────────
static void drawNewsScreen() {
    s_tft->fillScreen(COL_BG);

    char timeBuf[12] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12; else if (h == 0) h = 12;
        snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", h, ti.tm_min, ap);
    }
    drawTopbar(*s_tft, "< HOME | NEWS", timeBuf, g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    {
        static const char *DAYS[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
        struct tm lt;
        if (getLocalTime(&lt, 0)) {
            char dateBuf[28];
            snprintf(dateBuf, sizeof(dateBuf), "%s %s %d %d",
                     DAYS[lt.tm_wday], MONTHS[lt.tm_mon], lt.tm_mday, lt.tm_year + 1900);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(dateBuf, 4, TOPBAR_H + 3);
        }
    }
    if (s_itemCount > 0) {
        char cntBuf[24];
        snprintf(cntBuf, sizeof(cntBuf), "%s%d STORIES", s_cached ? "~" : "", s_itemCount);
        int cw = s_tft->textWidth(cntBuf);
        s_tft->setTextColor(s_cached ? COL_AMBER : g_themeColor, COL_BG);
        s_tft->drawString(cntBuf, SCREEN_W - cw - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int y = CONTENT_Y + 2;

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

    int visible = min(s_itemCount - s_scrollOff, NEWS_VISIBLE);

    for (int i = 0; i < visible; i++) {
        const NewsItem &item = s_items[s_scrollOff + i];
        int ry = y + i * NEWS_ROW_H;

        s_tft->setTextFont(FONT_SMALL);
        int dateW = item.when[0] ? s_tft->textWidth(item.when) : 0;
        if (dateW) {
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(item.when, SCREEN_W - dateW - 4, ry + 2);
        }

        String l1, l2;
        int line1W = SCREEN_W - 8 - (dateW ? dateW + 8 : 0);
        wordWrap(item.title, line1W, SCREEN_W - 16, l1, l2);

        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(l1, 4, ry);
        if (l2.length() > 0)
            s_tft->drawString(l2, 4, ry + 20);
    }

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
    strlcpy(s_errMsg, "", sizeof(s_errMsg));

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
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("news2_at", 0);
        s_cached    = false;
        s_itemCount = 0;
        s_scrollOff = 0;
        strlcpy(s_errMsg, "", sizeof(s_errMsg));
        showLoadingScreen();
        if (WiFi.isConnected()) {
            if (fetchNews()) saveCache();
        } else {
            strlcpy(s_errMsg, "No WiFi connection", sizeof(s_errMsg));
        }
        drawNewsScreen();
    }

    delay(20);
    return true;
}

void newsTrackballUp() {
    if (s_scrollOff > 0) { s_scrollOff--; drawNewsScreen(); }
}
void newsTrackballDown() {
    int maxOff = max(0, s_itemCount - NEWS_VISIBLE);
    if (s_scrollOff < maxOff) { s_scrollOff++; drawNewsScreen(); }
}
