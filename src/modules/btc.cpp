#include "btc.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <math.h>
#include <time.h>

#define BOARD_SDCARD_CS  39
#define KB_ADDR          0x55
#define CACHE_TTL_SEC    300    // 5 minutes
#define COIN_MAX         6
#define SPARK_POINTS     168    // 7 days × 24h hourly prices

// ── Data model ────────────────────────────────────────────────────────────────
struct CoinData {
    char   id[32];
    char   symbol[8];
    char   name[24];
    double priceUsd;
    float  change24h;
    float  change7d;
    float  spark[SPARK_POINTS];
    int    sparkCount;
    bool   valid;
};

static CoinData  s_coins[COIN_MAX];
static int       s_coinCount = 2;
static bool      s_fromCache = false;
static char      s_syncTime[10];
static int       s_fgValue   = -1;    // Fear & Greed 0-100, -1 = unknown
static char      s_fgLabel[16] = "";
static TFT_eSPI *s_tft = nullptr;

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Blocking readline (coin picker) ──────────────────────────────────────────
static String btcReadLine(const char *prompt) {
    String buf = "";
    s_tft->fillScreen(COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(prompt, 2, 10);
    auto redraw = [&]() {
        s_tft->fillRect(0, 26, SCREEN_W, 16, COL_BG);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(buf + "_", 2, 28);
    };
    redraw();
    while (true) {
        char k = readKeyboard();
        if (k == 0) { delay(20); continue; }
        if (k == '\r' || k == '\n') break;
        if ((k == 8 || k == 127) && buf.length() > 0) buf.remove(buf.length() - 1);
        else if (isprint((unsigned char)k) && buf.length() < 31) buf += k;
        redraw();
        delay(20);
    }
    return buf;
}

// ── Load coin IDs from NVS (defaults: bitcoin, ethereum) ─────────────────────
static bool addCoinId(const String &raw) {
    if (s_coinCount >= COIN_MAX) return false;
    String id = raw;
    id.trim();
    id.toLowerCase();
    if (id.isEmpty() || id.startsWith("#")) return false;
    int comment = id.indexOf('#');
    if (comment >= 0) {
        id = id.substring(0, comment);
        id.trim();
    }
    if (id.isEmpty()) return false;

    for (int i = 0; i < s_coinCount; i++) {
        if (id.equals(s_coins[i].id)) return false;
    }

    strlcpy(s_coins[s_coinCount].id, id.c_str(), sizeof(s_coins[s_coinCount].id));
    s_coinCount++;
    return true;
}

static bool loadCoinIdsFromFile(const char *path) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists(path)) { SD.end(); return false; }

    File f = SD.open(path, FILE_READ);
    if (!f) { SD.end(); return false; }

    s_coinCount = 0;
    String line = "";
    while (f.available() && s_coinCount < COIN_MAX) {
        char ch = (char)f.read();
        if (ch == '\r') continue;
        if (ch == '\n') {
            addCoinId(line);
            line = "";
        } else if (line.length() < 48) {
            line += ch;
        }
    }
    addCoinId(line);

    f.close();
    SD.end();
    return s_coinCount > 0;
}

static void loadCoinIds() {
    s_coinCount = 0;
    if (loadCoinIdsFromFile("/crypto.txt") || loadCoinIdsFromFile("/coins.txt")) return;

    for (int i = 0; i < COIN_MAX; i++) {
        char key[12];
        snprintf(key, sizeof(key), "btc_coin%d", i);
        addCoinId(nvsGetString(key));
    }

    if (s_coinCount == 0) {
        addCoinId("bitcoin");
        addCoinId("ethereum");
    }
}

// ── Format price with comma separators ───────────────────────────────────────
static void formatPrice(double price, char *out, int outLen) {
    char tmp[24];
    if (price < 0.001)
        snprintf(tmp, sizeof(tmp), "%.6f", price);
    else if (price < 1.0)
        snprintf(tmp, sizeof(tmp), "%.4f", price);
    else if (price < 100.0)
        snprintf(tmp, sizeof(tmp), "%.2f", price);
    else
        snprintf(tmp, sizeof(tmp), "%.0f", price);

    // Count integer digits (before decimal point)
    int intLen = 0;
    while (tmp[intLen] && tmp[intLen] != '.') intLen++;

    int o = 0;
    out[o++] = '$';
    for (int i = 0; tmp[i] && o < outLen - 1; i++) {
        if (i < intLen && i > 0 && (intLen - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

// ── SD cache ─────────────────────────────────────────────────────────────────
static bool saveCacheToSD(const String &json) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/cache")) SD.mkdir("/cache");
    File f = SD.open("/cache/btc.json", FILE_WRITE);
    if (!f) { SD.end(); return false; }
    f.print(json);
    f.close();
    SD.end();
    nvsPutInt("btc_cached_at", (int)time(nullptr));
    return true;
}

static bool loadCacheFromSD(String &json) {
    int cachedAt = nvsGetInt("btc_cached_at", 0);
    if (cachedAt == 0) return false;
    time_t now = time(nullptr);
    if (now > 1000000 && (now - cachedAt) > CACHE_TTL_SEC) return false;
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/cache/btc.json")) { SD.end(); return false; }
    File f = SD.open("/cache/btc.json", FILE_READ);
    if (!f) { SD.end(); return false; }
    json = "";
    while (f.available()) json += (char)f.read();
    f.close();
    SD.end();
    return json.length() > 10;
}

// ── JSON parser ───────────────────────────────────────────────────────────────
static bool parseCoinsJson(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.isNull() || arr.size() == 0) return false;

    bool anyValid = false;
    int arrSize = arr.size();
    for (int ai = 0; ai < arrSize; ai++) {
        const char *id = arr[ai]["id"].as<const char *>();
        if (!id) continue;

        for (int ci = 0; ci < s_coinCount; ci++) {
            if (strcmp(s_coins[ci].id, id) != 0) continue;

            const char *sym  = arr[ai]["symbol"].as<const char *>();
            const char *name = arr[ai]["name"].as<const char *>();
            strlcpy(s_coins[ci].symbol, sym  ? sym  : "???", sizeof(s_coins[ci].symbol));
            strlcpy(s_coins[ci].name,   name ? name : "???", sizeof(s_coins[ci].name));
            for (char *p = s_coins[ci].symbol; *p; p++) *p = toupper((unsigned char)*p);

            s_coins[ci].priceUsd  = arr[ai]["current_price"].as<double>();
            s_coins[ci].change24h = arr[ai]["price_change_percentage_24h"].as<float>();
            s_coins[ci].change7d  = arr[ai]["price_change_percentage_7d_in_currency"].as<float>();

            JsonArray spark = arr[ai]["sparkline_in_7d"]["price"].as<JsonArray>();
            int n = 0, sparkSize = spark.size();
            for (int si = 0; si < sparkSize && n < SPARK_POINTS; si++)
                s_coins[ci].spark[n++] = spark[si].as<float>();
            s_coins[ci].sparkCount = n;
            s_coins[ci].valid      = true;
            anyValid = true;
            break;
        }
    }

    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(s_syncTime, sizeof(s_syncTime), "%d:%02d %s", h, ti.tm_min, ap);
    } else {
        strlcpy(s_syncTime, "--:--", sizeof(s_syncTime));
    }

    return anyValid;
}

// ── Network fetch ─────────────────────────────────────────────────────────────
static bool fetchCoins() {
    char ids[256] = "";
    for (int i = 0; i < s_coinCount; i++) {
        if (i > 0) strlcat(ids, ",", sizeof(ids));
        strlcat(ids, s_coins[i].id, sizeof(ids));
    }
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.coingecko.com/api/v3/coins/markets"
        "?vs_currency=usd&ids=%s&order=market_cap_desc"
        "&sparkline=true&price_change_percentage=7d",
        ids);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(15000);
    http.addHeader("Accept", "application/json");
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String json = http.getString();
    http.end();

    if (!parseCoinsJson(json)) return false;
    saveCacheToSD(json);
    s_fromCache = false;
    return true;
}

// ── Fear & Greed Index ────────────────────────────────────────────────────────
static void fetchFearGreed() {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.alternative.me/fng/?limit=1");
    http.setTimeout(8000);
    if (http.GET() != 200) { http.end(); return; }
    String json = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    int val = doc["data"][0]["value"].as<int>();
    if (val < 0 || val > 100) return;
    s_fgValue = val;
    const char *cls = doc["data"][0]["value_classification"] | "";
    strlcpy(s_fgLabel, cls, sizeof(s_fgLabel));
    for (char *p = s_fgLabel; *p; p++) *p = toupper((unsigned char)*p);
}

static uint16_t fgColor(int v) {
    return g_themeColor;
}

// ── Load: network then cache ──────────────────────────────────────────────────
static void loadData() {
    for (int i = 0; i < COIN_MAX; i++) s_coins[i].valid = false;
    s_fromCache = false;

    if (WiFi.isConnected()) {
        if (fetchCoins()) {
            fetchFearGreed();
            return;
        }
    }
    String json;
    if (loadCacheFromSD(json) && parseCoinsJson(json)) {
        s_fromCache = true;
    }
}

// ── Sparkline renderer ────────────────────────────────────────────────────────
static void drawSparkline(int x, int y, int w, int h, const CoinData &c) {
    if (c.sparkCount < 2) return;

    // Clear the region first so previous-frame lines don't bleed through on refresh
    s_tft->fillRect(x, y, w, h + 2, COL_BG);

    float mn = c.spark[0], mx = c.spark[0];
    for (int i = 1; i < c.sparkCount; i++) {
        if (c.spark[i] < mn) mn = c.spark[i];
        if (c.spark[i] > mx) mx = c.spark[i];
    }
    float range = mx - mn;
    if (range < 1e-6f) range = 1.0f;

    uint16_t col = c.change7d >= 0 ? g_themeColor : COL_RED;

    int px = -1, py = -1;
    for (int i = 0; i < c.sparkCount; i++) {
        int cx = x + (int)((float)i * (w - 1) / (c.sparkCount - 1));
        int cy = y + h - 1 - (int)(((c.spark[i] - mn) / range) * (h - 2));
        if (cy < y)         cy = y;
        if (cy > y + h - 1) cy = y + h - 1;
        if (px >= 0) s_tft->drawLine(px, py, cx, cy, col);
        px = cx; py = cy;
    }
    // Baseline
    s_tft->drawFastHLine(x, y + h, w, g_themeColor);
}

// ── Screen drawing ────────────────────────────────────────────────────────────
// After SD.end() the SPI bus can be left in a state the TFT doesn't accept.
// Explicitly deassert SD CS and re-issue a TFT-speed SPI transaction so the
// next fillScreen / drawXxx works reliably (mirrors world.cpp which has no SD).
static void spiReinitForTFT() {
    digitalWrite(BOARD_SDCARD_CS, HIGH);
    SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
    SPI.endTransaction();
}

static void drawBtcScreen() {
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
    drawTopbar(*s_tft, "< HOME | CRYPTO", rightBuf, g_themeColor);

    // Status bar
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("COINGECKO", 4, TOPBAR_H + 3);

    // Fear & Greed centre
    if (s_fgValue >= 0) {
        char fgBuf[24];
        snprintf(fgBuf, sizeof(fgBuf), "F&G:%d %s", s_fgValue, s_fgLabel);
        int fw = s_tft->textWidth(fgBuf);
        s_tft->setTextColor(fgColor(s_fgValue), COL_BG);
        s_tft->drawString(fgBuf, (SCREEN_W - fw) / 2, TOPBAR_H + 3);
    }

    bool anyValid = false;
    for (int i = 0; i < s_coinCount; i++) anyValid |= s_coins[i].valid;

    if (anyValid) {
        char syncBuf[24];
        snprintf(syncBuf, sizeof(syncBuf), "%s %s", s_fromCache ? "CACHED" : "LIVE", s_syncTime);
        int sw = s_tft->textWidth(syncBuf);
        s_tft->setTextColor(s_fromCache ? COL_AMBER : g_themeColor, COL_BG);
        s_tft->drawString(syncBuf, SCREEN_W - sw - 4, TOPBAR_H + 3);
    }
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int contentY = TOPBAR_H + STATUSBAR_H;

    if (!anyValid) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("NO DATA", SCREEN_W / 2, contentY + 40, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("OFFLINE - no cache available", SCREEN_W / 2, contentY + 62, FONT_SMALL);
        s_tft->drawCentreString("Q=home  R=retry  C=coins", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
        return;
    }

    // Content split: two equal rows
    int contentH = SCREEN_H - contentY - 14;  // 14 = hint bar
    int rowH     = contentH / s_coinCount;

    // fillScreen is unreliable after SD SPI ops — secondary fillRect clears what it missed.
    s_tft->fillRect(0, contentY, SCREEN_W, SCREEN_H - contentY, COL_BG);

    // Compact list mode for SD/NVS coin watchlists larger than two coins.
    if (s_coinCount > 2) {
        int rowHCompact = contentH / s_coinCount;
        if (rowHCompact < 24) rowHCompact = 24;

        for (int i = 0; i < s_coinCount; i++) {
            int ry = contentY + i * rowHCompact;
            const CoinData &c = s_coins[i];

            if (i > 0) s_tft->drawFastHLine(0, ry, SCREEN_W, g_themeColor);

            if (!c.valid) {
                s_tft->setTextFont(FONT_SMALL);
                s_tft->setTextColor(g_themeColor, COL_BG);
                char buf[36];
                snprintf(buf, sizeof(buf), "%.30s: NOT FOUND", s_coins[i].id);
                s_tft->drawString(buf, 4, ry + 8);
                continue;
            }

            char priceBuf[18];
            char chg24[12];
            char chg7d[12];
            formatPrice(c.priceUsd, priceBuf, sizeof(priceBuf));
            snprintf(chg24, sizeof(chg24), "%+.1f%%", c.change24h);
            snprintf(chg7d, sizeof(chg7d), "%+.1f%%", c.change7d);

            s_tft->setTextFont(FONT_MED);
            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(c.symbol, 4, ry + 4);

            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString(priceBuf, 70, ry + 4);

            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(c.change24h >= 0 ? g_themeColor : COL_RED, COL_BG);
            s_tft->drawString(chg24, 190, ry + 4);
            s_tft->setTextColor(c.change7d >= 0 ? g_themeColor : COL_RED, COL_BG);
            s_tft->drawString(chg7d, 252, ry + 4);

            s_tft->setTextColor(g_themeColor, COL_BG);
            s_tft->drawString("24H", 190, ry + 16);
            s_tft->drawString("7D", 252, ry + 16);
        }

        int ya = SCREEN_H - BOTTOMBAR_H;
        s_tft->drawFastHLine(0, ya, SCREEN_W, g_themeColor);
        s_tft->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, g_themeColor);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("Q=home  R=refresh  C=coins", SCREEN_W / 2, ya + 3, FONT_SMALL);
        return;
    }
    for (int i = 0; i < s_coinCount; i++) {
        int ry = contentY + i * rowH;
        const CoinData &c = s_coins[i];

        if (i > 0) s_tft->drawFastHLine(0, ry, SCREEN_W, g_themeColor);

        if (!c.valid) {
            s_tft->setTextFont(FONT_SMALL);
            s_tft->setTextColor(g_themeColor, COL_BG);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.30s: NOT FOUND", s_coins[i].id);
            s_tft->drawString(buf, 4, ry + rowH / 2 - 4);
            continue;
        }

        // ── Left text block (x = 0..168) ─────────────────────────────────────
        // Symbol
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(c.symbol, 4, ry + 4);
        int symW = s_tft->textWidth(c.symbol);

        // Name (grey, same line, right of symbol)
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        char truncName[18];
        strlcpy(truncName, c.name, sizeof(truncName));
        s_tft->drawString(truncName, 4 + symW + 4, ry + 8);

        // Price
        char priceBuf[20];
        formatPrice(c.priceUsd, priceBuf, sizeof(priceBuf));
        s_tft->setTextFont(FONT_LARGE);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString(priceBuf, 4, ry + 22);

        const int changeLabelX = 126;
        const int changeValueRight = changeLabelX - 6;

        // 24h change
        char chg24[16];
        snprintf(chg24, sizeof(chg24), "%+.2f%%", c.change24h);
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(c.change24h >= 0 ? g_themeColor : COL_RED, COL_BG);
        s_tft->drawString(chg24, changeValueRight - s_tft->textWidth(chg24), ry + 52);

        // "24H" label
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString("24H", changeLabelX, ry + 56);

        // 7d change
        char chg7d[16];
        snprintf(chg7d, sizeof(chg7d), "%+.2f%%", c.change7d);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(c.change7d >= 0 ? g_themeColor : COL_RED, COL_BG);
        s_tft->drawString(chg7d, changeValueRight - s_tft->textWidth(chg7d), ry + 70);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString("7D", changeLabelX, ry + 70);

        // ── Divider between text and sparkline ────────────────────────────────
        s_tft->drawFastVLine(170, ry + 2, rowH - 4, g_themeColor);

        // ── Right: 7-day sparkline (x = 174..315) ────────────────────────────
        int sx = 174, sy = ry + 8;
        int sw = SCREEN_W - sx - 4;      // ~142px wide
        int sh = rowH - 18;              // ~77px tall
        if (sh > 4) drawSparkline(sx, sy, sw, sh, c);

        // Sparkline period label
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString("7D", sx, ry + rowH - 12);
    }

    // ── Hint bar ─────────────────────────────────────────────────────────────
    int ya = SCREEN_H - BOTTOMBAR_H;
    s_tft->drawFastHLine(0, ya, SCREEN_W, g_themeColor);
    s_tft->drawFastHLine(0, SCREEN_H - 1, SCREEN_W, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("Q=home  R=refresh  C=coins", SCREEN_W / 2, ya + 3, FONT_SMALL);
}

// ── Coin picker ───────────────────────────────────────────────────────────────
static void doCoinPicker() {
    char oldIds[COIN_MAX][32];
    int oldCount = s_coinCount;
    for (int i = 0; i < COIN_MAX; i++) {
        oldIds[i][0] = '\0';
        if (i < oldCount) strlcpy(oldIds[i], s_coins[i].id, sizeof(oldIds[i]));
        char key[12];
        snprintf(key, sizeof(key), "btc_coin%d", i);
        nvsPutString(key, "");
    }

    s_coinCount = 0;
    for (int i = 0; i < COIN_MAX; i++) {
        char prompt[60];
        snprintf(prompt, sizeof(prompt), "Coin %d ID blank=keep/skip:", i + 1);
        String val = btcReadLine(prompt);
        val.trim();
        if (val.isEmpty() && i < oldCount) val = oldIds[i];
        if (val.isEmpty()) continue;
        val.toLowerCase();
        if (!addCoinId(val)) continue;

        char key[12];
        snprintf(key, sizeof(key), "btc_coin%d", s_coinCount - 1);
        nvsPutString(key, val);
    }

    if (s_coinCount == 0) {
        addCoinId("bitcoin");
        addCoinId("ethereum");
    }
    nvsPutInt("btc_cached_at", 0);
}

// -- Public API ────────────────────────────────────────────────────────────────
void btcInit(TFT_eSPI &tft) {
    s_tft = &tft;
    strlcpy(s_syncTime, "--:--", sizeof(s_syncTime));
    for (int i = 0; i < COIN_MAX; i++) s_coins[i].valid = false;
    s_fromCache = false;

    // Show loading screen BEFORE any SD operations so the TFT clear runs
    // while the SPI bus is in a known TFT state (mirrors world.cpp pattern).
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | CRYPTO", "", g_themeColor);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("Fetching crypto data...", SCREEN_W / 2, SCREEN_H / 2, FONT_SMALL);

    // SD ops: read coin list + check cache
    loadCoinIds();
    String cachedJson;
    bool cacheHit = loadCacheFromSD(cachedJson) && parseCoinsJson(cachedJson);
    if (cacheHit) s_fromCache = true;

    // Restore SPI for TFT after SD operations
    spiReinitForTFT();

    if (cacheHit) {
        drawBtcScreen();
        if (WiFi.isConnected()) {
            if (fetchCoins()) fetchFearGreed();  // fetchCoins → saveCacheToSD → SD ops
            spiReinitForTFT();
            drawBtcScreen();
        }
        return;
    }

    // No cache — fetch live, then draw
    if (WiFi.isConnected()) {
        if (fetchCoins()) fetchFearGreed();  // fetchCoins → saveCacheToSD → SD ops
        spiReinitForTFT();
    }
    drawBtcScreen();
}

bool btcLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) return false;

    if (key == 'r' || key == 'R') {
        nvsPutInt("btc_cached_at", 0);
        loadData();       // SD ops inside
        spiReinitForTFT();
        drawBtcScreen();
    }

    if (key == 'c' || key == 'C') {
        doCoinPicker();
        loadData();       // SD ops inside
        spiReinitForTFT();
        drawBtcScreen();
    }

    delay(20);
    return true;
}
