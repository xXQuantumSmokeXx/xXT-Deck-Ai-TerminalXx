#include "news.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ── Constants ─────────────────────────────────────────────────────────────────
#define KB_ADDR         0x55
#define NEWS_MAX_ITEMS  20      // 5 items × 4 feeds
#define NEWS_VISIBLE    4
#define NEWS_ROW_H      44
#define NEWS_CACHE_TTL  3600    // 1 hour

// ── Feed definitions ──────────────────────────────────────────────────────────
struct FeedDef {
    const char *badge;
    const char *url;
    uint16_t    color;
};

static const FeedDef FEEDS[] = {
    { "[CDC]", "https://tools.cdc.gov/api/v2/resources/media/rss.rss",  COL_RED   },
    { "[WHO]", "https://www.who.int/rss-feeds/news-releases.xml",        COL_AMBER },
    { "[ABC]", "https://abcnews.go.com/abcnews/internationalheadlines",   COL_WHITE },
    { "[WLD]", "https://feeds.bbci.co.uk/news/world/rss.xml",           COL_WHITE },
};
static const int FEED_COUNT = 4;

// ── State ─────────────────────────────────────────────────────────────────────
struct NewsItem {
    char     badge[8];
    char     title[100];
    uint16_t color;
};

static TFT_eSPI  *s_tft        = nullptr;
static NewsItem   s_items[NEWS_MAX_ITEMS];
static int        s_itemCount   = 0;
static int        s_scrollOff   = 0;
static bool       s_dataCached  = false;
static char       s_errMsg[52]  = "";

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

// Badge color: CDC=red, WHO=amber, ABC/WLD=white, everything else=theme
static uint16_t feedBadgeColor(const char *badge) {
    if (strcmp(badge, "[CDC]") == 0) return COL_RED;
    if (strcmp(badge, "[WHO]") == 0) return COL_AMBER;
    if (strcmp(badge, "[ABC]") == 0) return COL_WHITE;
    if (strcmp(badge, "[WLD]") == 0) return COL_WHITE;
    return g_themeColor;
}

// ── XML title extractor (handles RSS2 <item> and Atom <entry>) ────────────────
static void parseTitlesFromRss(const String &xml, const char *badge, uint16_t color, int maxItems) {
    int pos   = 0;
    int added = 0;

    while (s_itemCount < NEWS_MAX_ITEMS && added < maxItems) {
        int iPos = xml.indexOf("<item",  pos);
        int ePos = xml.indexOf("<entry", pos);
        if (iPos < 0 && ePos < 0) break;

        bool isEntry = (ePos >= 0 && (iPos < 0 || ePos < iPos));
        String closeTag = isEntry ? "</entry>" : "</item>";
        int blockStart  = isEntry ? ePos : iPos;

        int contentStart = xml.indexOf('>', blockStart);
        if (contentStart < 0) break;
        contentStart++;

        int end = xml.indexOf(closeTag, contentStart);
        if (end < 0) break;

        String block = xml.substring(contentStart, end);

        // Extract <title> — supports plain text and CDATA
        String title;
        int ts = block.indexOf("<title");
        if (ts >= 0) {
            int gt = block.indexOf('>', ts);
            if (gt >= 0) {
                int inner = gt + 1;
                if (block.substring(inner, inner + 9) == "<![CDATA[") {
                    inner += 9;
                    int te = block.indexOf("]]>", inner);
                    if (te > inner) title = block.substring(inner, te);
                } else {
                    int te = block.indexOf("</title>", inner);
                    if (te > inner) title = block.substring(inner, te);
                }
            }
        }

        title.trim();
        if (title.length() >= 3) {
            NewsItem &ni = s_items[s_itemCount++];
            strlcpy(ni.badge, badge, sizeof(ni.badge));
            strlcpy(ni.title, title.c_str(), sizeof(ni.title));
            decodeEntities(ni.title, sizeof(ni.title));
            ni.color = color;
            added++;
        }

        pos = end + closeTag.length();
    }
}

// ── NVS cache ─────────────────────────────────────────────────────────────────
static void saveCache() {
    String packed;
    for (int i = 0; i < s_itemCount; i++) {
        if (i > 0) packed += '\x02';
        packed += s_items[i].badge;
        packed += '\x01';
        packed += s_items[i].title;
    }
    nvsPutString("news_d",  packed);
    nvsPutInt(   "news_n",  s_itemCount);
    nvsPutInt(   "news_at", (int32_t)time(nullptr));
}

static bool loadCache() {
    int32_t ts = nvsGetInt("news_at");
    if (ts == 0) return false;
    if ((int32_t)time(nullptr) - ts > NEWS_CACHE_TTL) return false;

    int n = nvsGetInt("news_n");
    if (n <= 0 || n > NEWS_MAX_ITEMS) return false;

    String packed = nvsGetString("news_d");
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
            strlcpy(item.badge, chunk.substring(0, f1).c_str(), sizeof(item.badge));
            strlcpy(item.title, chunk.substring(f1 + 1).c_str(), sizeof(item.title));
            item.color = feedBadgeColor(item.badge);
        }
        pos = end + 1;
    }
    s_dataCached = true;
    return (s_itemCount > 0);
}

// ── Fetch one RSS feed directly ───────────────────────────────────────────────
static void fetchFeed(int idx) {
    const FeedDef &feed = FEEDS[idx];

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, feed.url);
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("User-Agent", "Mozilla/5.0 (compatible; ESP32)");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) { http.end(); return; }

    String xml = http.getString();
    http.end();

    parseTitlesFromRss(xml, feed.badge, feedBadgeColor(feed.badge), 5);
}

// ── Word-wrap helper — splits title across two lines ─────────────────────────
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
        while (l1.length() > 1 && s_tft->textWidth(l1) > maxW1) l1.remove(l1.length()-1);
        l2 = ""; return;
    }
    l1 = full.substring(0, split);
    l2 = full.substring(split + 1);
    if (s_tft->textWidth(l2) > maxW2) {
        while (l2.length() > 2 && s_tft->textWidth(l2 + "..") > maxW2)
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
    // Date left
    {
        static const char *DAYS[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *MONTHS[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            char dateBuf[28];
            snprintf(dateBuf, sizeof(dateBuf), "%s %s %d %d",
                     DAYS[ti.tm_wday], MONTHS[ti.tm_mon], ti.tm_mday, ti.tm_year + 1900);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(dateBuf, 4, TOPBAR_H + 3);
        }
    }
    // Story count right
    if (s_itemCount > 0) {
        char cntBuf[24];
        snprintf(cntBuf, sizeof(cntBuf), "%s%d STORIES", s_dataCached ? "~" : "", s_itemCount);
        int cw = s_tft->textWidth(cntBuf);
        s_tft->setTextColor(s_dataCached ? COL_AMBER : g_themeColor, COL_BG);
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
        int idx = s_scrollOff + i;
        const NewsItem &item = s_items[idx];
        int ry = y + i * NEWS_ROW_H;

        s_tft->setTextFont(FONT_MED);
        int bw = s_tft->textWidth(item.badge) + 4;

        String l1, l2;
        wordWrap(item.title, SCREEN_W - bw - 4, SCREEN_W - 16, l1, l2);

        s_tft->setTextDatum(0);
        s_tft->setTextColor(item.color, COL_BG);
        s_tft->drawString(item.badge, 4, ry);

        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(l1, bw + 4, ry);
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
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("Fetching headlines...", SCREEN_W / 2, CONTENT_Y + 36, FONT_SMALL);
}

static void fetchAllFeeds() {
    s_itemCount = 0;
    strlcpy(s_errMsg, "", sizeof(s_errMsg));

    for (int i = 0; i < FEED_COUNT; i++) {
        s_tft->fillRect(0, CONTENT_Y + 50, SCREEN_W, 18, COL_BG);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        char prog[40];
        snprintf(prog, sizeof(prog), "%s  (%d / %d)", FEEDS[i].badge, i + 1, FEED_COUNT);
        s_tft->drawCentreString(prog, SCREEN_W / 2, CONTENT_Y + 52, FONT_SMALL);

        fetchFeed(i);
    }

    if (s_itemCount == 0)
        strlcpy(s_errMsg, "No stories fetched - check WiFi", sizeof(s_errMsg));
}

// ── Boot-time cache warming ──────────────────────────────────────────────────
// Called once at startup before the user navigates to any screen.
// Populates NVS cache so newsInit() always finds data ready.
void newsWarmCache() {
    if (!WiFi.isConnected()) return;
    fetchAllFeeds();         // fetches RSS feeds
    if (s_itemCount > 0) saveCache();  // saves to NVS
}

// ── Public API ────────────────────────────────────────────────────────────────
void newsInit(TFT_eSPI &tft) {
    s_tft        = &tft;
    s_itemCount  = 0;
    s_scrollOff  = 0;
    s_dataCached = false;
    strlcpy(s_errMsg, "", sizeof(s_errMsg));

    if (loadCache()) {
        drawNewsScreen();
        return;
    }

    showLoadingScreen();

    if (WiFi.isConnected()) {
        fetchAllFeeds();
        if (s_itemCount > 0) saveCache();
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
        nvsPutInt("news_at", 0);
        s_dataCached = false;
        s_itemCount  = 0;
        s_scrollOff  = 0;
        showLoadingScreen();
        if (WiFi.isConnected()) {
            fetchAllFeeds();
            if (s_itemCount > 0) saveCache();
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
