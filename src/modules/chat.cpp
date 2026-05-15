#include "chat.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../config/nvs_config.h"
#include "../persona/persona_mgr.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>

// ── Board pins ────────────────────────────────────────────────────────────────
#define KB_ADDR        0x55

// ── Layout ────────────────────────────────────────────────────────────────────
#define CHAT_LINE_H     20
#define CHAT_VISIBLE    ((SCREEN_H - 22 - TOPBAR_H - STATUSBAR_H) / CHAT_LINE_H)
#define CHAT_INPUT_Y    (SCREEN_H - 22)
#define CHAT_HISTORY_SZ 200

// ── Markdown stripper ─────────────────────────────────────────────────────────
static String stripMarkdown(const String &in) {
    String out;
    out.reserve(in.length());
    int len = in.length();
    int i = 0;
    while (i < len) {
        if (i + 2 < len && in[i] == '`' && in[i+1] == '`' && in[i+2] == '`') {
            i += 3;
            while (i < len && in[i] != '\n') i++;
            continue;
        }
        if (in[i] == '*') {
            i += (i + 1 < len && in[i+1] == '*') ? 2 : 1;
            continue;
        }
        if (in[i] == '#') {
            while (i < len && in[i] == '#') i++;
            if (i < len && in[i] == ' ') i++;
            continue;
        }
        if ((i == 0 || in[i-1] == '\n') && in[i] == '-' && i + 1 < len && in[i+1] == ' ') {
            out += '\xBB';
            i += 2;
            continue;
        }
        out += in[i++];
    }
    return out;
}

// ── Conversation context ──────────────────────────────────────────────────────
#define CTX_MAX_PAIRS  6
#define CTX_SLOTS      (CTX_MAX_PAIRS * 2)
#define CTX_MSG_MAX    500

struct CtxMsg { String role; String content; };
static CtxMsg s_ctx[CTX_SLOTS];
static int    s_ctxCount = 0;

static void ctxAdd(const String &user, const String &asst) {
    if (s_ctxCount >= CTX_SLOTS) {
        for (int i = 0; i < CTX_SLOTS - 2; i++) s_ctx[i] = s_ctx[i + 2];
        s_ctxCount = CTX_SLOTS - 2;
    }
    String u = user, a = asst;
    if ((int)u.length() > CTX_MSG_MAX) u = u.substring(0, CTX_MSG_MAX);
    if ((int)a.length() > CTX_MSG_MAX) a = a.substring(0, CTX_MSG_MAX);
    s_ctx[s_ctxCount++] = { "user",      u };
    s_ctx[s_ctxCount++] = { "assistant", a };
}

static void ctxClear() { s_ctxCount = 0; }

// ── Chat history ──────────────────────────────────────────────────────────────
struct ChatLine { String text; uint16_t color; };
static ChatLine s_history[CHAT_HISTORY_SZ];
static int      s_histCount = 0;
static int      s_scrollOff = 0;
static String   s_inputBuf  = "";
static String   s_aiTarget  = "";
static String   s_assist[2] = {"", ""};


// ── Rendering ─────────────────────────────────────────────────────────────────
static TFT_eSPI *s_tft = nullptr;

static void redrawChatArea() {
    int top = TOPBAR_H + STATUSBAR_H;
    s_tft->fillRect(0, top, SCREEN_W - 4, CHAT_INPUT_Y - top - 1, COL_BG);
    s_tft->setTextFont(FONT_MED);
    int lastLine  = s_histCount - 1 - s_scrollOff;
    int firstLine = lastLine - CHAT_VISIBLE + 1;
    for (int i = 0; i < CHAT_VISIBLE; i++) {
        int idx = firstLine + i;
        if (idx < 0 || idx >= s_histCount) continue;
        s_tft->setTextColor(s_history[idx].color, COL_BG);
        s_tft->drawString(s_history[idx].text, 2, top + 2 + i * CHAT_LINE_H);
    }
    s_tft->fillRect(SCREEN_W - 4, top, 4, CHAT_INPUT_Y - top - 1, COL_BG);
    if (s_histCount > CHAT_VISIBLE) {
        int barH   = max(4, (CHAT_INPUT_Y - top - 1) * CHAT_VISIBLE / s_histCount);
        int maxOff = s_histCount - CHAT_VISIBLE;
        int barY   = top + (CHAT_INPUT_Y - top - 1 - barH) * (maxOff - s_scrollOff) / maxOff;
        s_tft->fillRect(SCREEN_W - 4, barY, 4, barH, g_themeColor);
    }
}

static void redrawInput() {
    PersonaDef *p = personaMgrGet();
    s_tft->fillRect(0, CHAT_INPUT_Y, SCREEN_W, SCREEN_H - CHAT_INPUT_Y, COL_INPUT_BG);
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_INPUT_BG);
    String display = "> " + s_inputBuf + "_";
    if (display.length() > 28) display = display.substring(display.length() - 28);
    s_tft->drawString(display, 2, CHAT_INPUT_Y + 4);
    // Accent line above input bar
    s_tft->drawFastHLine(0, CHAT_INPUT_Y - 1, SCREEN_W, g_themeColor);
}

static void pushLine(const String &text, uint16_t color) {
    if (s_histCount < CHAT_HISTORY_SZ) {
        s_history[s_histCount++] = { text, color };
    } else {
        for (int i = 0; i < CHAT_HISTORY_SZ - 1; i++) s_history[i] = s_history[i + 1];
        s_history[CHAT_HISTORY_SZ - 1] = { text, color };
    }
    if (s_scrollOff == 0) redrawChatArea();
}

static void pushWrapped(const String &prefix, const String &text, uint16_t color) {
    s_tft->setTextFont(FONT_MED);
    String full = prefix + text;
    while (full.length() > 0) {
        if (s_tft->textWidth(full) <= SCREEN_W - 8) { pushLine(full, color); break; }
        int lo = 1, hi = full.length();
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (s_tft->textWidth(full.substring(0, mid)) <= SCREEN_W - 8) lo = mid;
            else hi = mid - 1;
        }
        int cut = lo, space = cut;
        while (space > 1 && full[space] != ' ') space--;
        if (space > 1) cut = space;
        pushLine(full.substring(0, cut), color);
        String rest = full.substring(cut);
        if (rest.length() > 0 && rest[0] == ' ') rest = rest.substring(1);
        full = "  " + rest;
    }
}

static void scrollUp()       { int m = max(0, s_histCount - CHAT_VISIBLE); if (s_scrollOff < m) { s_scrollOff++; redrawChatArea(); } }
static void scrollDown()     { if (s_scrollOff > 0) { s_scrollOff--; redrawChatArea(); } }
static void scrollToBottom() { s_scrollOff = 0; redrawChatArea(); }

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Blocking confirm prompt ("Switch to X? y/n") ─────────────────────────────
static bool confirmPrompt(const String &msg) {
    s_tft->fillRect(0, CHAT_INPUT_Y - 20, SCREEN_W, 20, COL_AMBER);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_BG, COL_AMBER);
    String disp = msg + " y/n";
    s_tft->drawString(disp, 4, CHAT_INPUT_Y - 17);
    while (true) {
        char k = readKeyboard();
        if (k == 'y' || k == 'Y') return true;
        if (k == 'n' || k == 'N' || k == 27) return false;
        delay(20);
    }
}

// ── Blocking readLine ─────────────────────────────────────────────────────────
static String readLine(const String &prompt, bool mask = false) {
    String buf = "";
    s_tft->fillScreen(COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    if ((int)prompt.length() > 52) {
        s_tft->drawString(prompt.substring(0, 52), 2, 10);
        s_tft->drawString(prompt.substring(52), 2, 26);
    } else {
        s_tft->drawString(prompt, 2, 10);
    }
    auto redrawRL = [&]() {
        s_tft->fillRect(0, 42, SCREEN_W, 20, COL_BG);
        s_tft->setTextColor(g_themeColor, COL_BG);
        String m = mask ? String(buf.length(), '*') : buf;
        s_tft->drawString(m + "_", 2, 44);
    };
    redrawRL();
    while (true) {
        char key = readKeyboard();
        if (key == 0) { delay(20); continue; }
        if (key == '\r' || key == '\n') break;
        if ((key == 8 || key == 127) && buf.length() > 0) buf.remove(buf.length() - 1);
        else if (isprint((unsigned char)key) && buf.length() < 80) buf += key;
        redrawRL();
        delay(20);
    }
    return buf;
}

// ── Topbar ────────────────────────────────────────────────────────────────────
static void drawChatTopbar() {
    PersonaDef *p = personaMgrGet();
    char title[56];
    // Show slot indicator if more than one persona loaded
    if (personaMgrLoadedCount() > 1) {
        snprintf(title, sizeof(title), ">> %s [%d/%d]",
                 p->name, personaMgrCurrentSlot() + 1, personaMgrLoadedCount());
    } else {
        snprintf(title, sizeof(title), ">> %s", p->name);
    }
    String tgt = s_aiTarget; tgt.toUpperCase();
    char rightLabel[16];
    snprintf(rightLabel, sizeof(rightLabel), "%s Q=home", tgt.c_str());
    drawTopbar(*s_tft, title, rightLabel, g_themeColor);
    drawStatusBar(*s_tft, WiFi.isConnected(), false, p->title[0] ? p->title : p->name, 0);
}

// ── HTTP send ─────────────────────────────────────────────────────────────────
static void sendMessage(const String &msg) {
    PersonaDef *p = personaMgrGet();
    String base = nvsGetString("server_url");
    pushWrapped("YOU: ", msg, COL_YOU);
    pushLine("...", COL_SYS);
    scrollToBottom();
    redrawInput();

    if (!WiFi.isConnected()) {
        s_history[max(0, s_histCount - 1)] = { "ERR: not connected", COL_SYS };
        redrawChatArea(); return;
    }
    if (base.isEmpty()) {
        if (s_histCount > 0 && s_history[s_histCount-1].text == "...") s_histCount--;
        pushLine("ERR: no URL. Type seturl", COL_SYS);
        scrollToBottom(); return;
    }
    if (s_aiTarget.isEmpty()) {
        if (s_histCount > 0 && s_history[s_histCount-1].text == "...") s_histCount--;
        pushLine("ERR: no assistant. Type setassist1", COL_SYS);
        scrollToBottom(); return;
    }

    String endpoint = base + "/" + s_aiTarget + "/simple";
    bool   isHttps  = base.startsWith("https://");

    JsonDocument reqDoc;
    reqDoc["message"] = msg;
    if (p->systemPrompt.length() > 0) reqDoc["system"] = p->systemPrompt;
    JsonArray ctx = reqDoc["context"].to<JsonArray>();
    for (int i = 0; i < s_ctxCount; i++) {
        JsonObject m = ctx.add<JsonObject>();
        m["role"]    = s_ctx[i].role;
        m["content"] = s_ctx[i].content;
    }
    String body;
    serializeJson(reqDoc, body);

    int    code = -1;
    String raw  = "";

    if (isHttps) {
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        http.begin(client, endpoint);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("ngrok-skip-browser-warning", "true");
        http.setTimeout(60000);
        code = http.POST(body);
        if (code == 200) raw = http.getString();
        http.end();
    } else {
        HTTPClient http;
        http.begin(endpoint);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(60000);
        code = http.POST(body);
        if (code == 200) raw = http.getString();
        http.end();
    }

    if (s_histCount > 0 && s_history[s_histCount - 1].text == "...") s_histCount--;

    if (code == 200) {
        JsonDocument res;
        if (deserializeJson(res, raw)) {
            pushLine("ERR: bad JSON", COL_SYS);
        } else {
            String reply = stripMarkdown(res["response"].as<String>());
            ctxAdd(msg, reply);
            String label = s_aiTarget.isEmpty() ? String(p->name) : s_aiTarget;
            label.toUpperCase();
            pushWrapped(label + ": ", reply, g_themeColor);
        }
    } else {
        pushLine(code > 0 ? "ERR: HTTP " + String(code) : "ERR: connection failed", COL_SYS);
    }
    scrollToBottom();
}

// ── Persona switch with confirm ───────────────────────────────────────────────
static void switchPersona() {
    if (personaMgrLoadedCount() <= 1) {
        pushLine("Only 1 persona loaded.", COL_SYS);
        pushLine("Add /personas/p2.txt to SD.", COL_SYS);
        scrollToBottom(); redrawInput();
        return;
    }

    if (s_histCount > 0) {
        if (!confirmPrompt("Switch persona? History clears.")) {
            redrawChatArea(); redrawInput();
            return;
        }
    }

    if (personaMgrNext()) {
        s_histCount = 0; s_scrollOff = 0; ctxClear();
        s_tft->fillScreen(COL_BG);
        drawChatTopbar();
        PersonaDef *p = personaMgrGet();
        pushLine(">> " + String(p->name), g_themeColor);
        if (p->title[0]) pushLine(p->title, g_themeColor);
        redrawChatArea();
        redrawInput();
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void chatInit(TFT_eSPI &tft) {
    s_tft       = &tft;
    s_histCount = 0;
    s_scrollOff = 0;
    s_inputBuf  = "";
    s_assist[0] = nvsGetString("assist1", ""); s_assist[0].toLowerCase();
    s_assist[1] = nvsGetString("assist2", ""); s_assist[1].toLowerCase();
    s_aiTarget  = nvsGetString("ai_target", s_assist[0]); s_aiTarget.toLowerCase();
    ctxClear();

    tft.fillScreen(COL_BG);
    drawChatTopbar();
    redrawInput();

    PersonaDef *p = personaMgrGet();
    pushLine(">> " + String(p->name) + (p->title[0] ? " - " + String(p->title) : ""), g_themeColor);
    pushWrapped("", "seturl setwifi setassist1 setassist2 persona clear", g_themeColor);
    redrawChatArea();
    redrawInput();
}

bool chatLoop(TFT_eSPI &tft) {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    // Ctrl+Q, ESC, or Q with empty input → return to home
    if (key == 17 || key == 27) return false;
    if ((key == 'q' || key == 'Q') && s_inputBuf.isEmpty()) return false;

    if (key == '\r' || key == '\n') {
        String msg = s_inputBuf; msg.trim();
        s_inputBuf = "";
        redrawInput();

        if (msg == "clear") {
            s_histCount = 0; s_scrollOff = 0; ctxClear();
            redrawChatArea(); redrawInput();
        } else if (msg.startsWith("setassist1") || msg.startsWith("setassist2")) {
            int idx = msg.startsWith("setassist1") ? 0 : 1;
            String slug;
            if (msg.length() > 10 && msg[10] == ' ') {
                slug = msg.substring(11); slug.trim();
            } else {
                slug = readLine(idx == 0 ? "Assistant 1 slug:" : "Assistant 2 slug:");
                slug.trim();
                tft.fillScreen(COL_BG); drawChatTopbar();
            }
            if (slug.length() > 0) {
                slug.toLowerCase();
                s_assist[idx] = slug;
                nvsPutString(idx == 0 ? "assist1" : "assist2", slug);
                pushLine("Assist" + String(idx + 1) + ": " + slug, COL_SYS);
            }
            scrollToBottom(); redrawInput();
        } else if (msg == "persona") {
            switchPersona();
        } else if (msg == "seturl") {
            String url = readLine("Enter server URL:");
            url.trim();
            if (url.endsWith("/")) url.remove(url.length() - 1);
            nvsPutString("server_url", url);
            tft.fillScreen(COL_BG);
            drawChatTopbar();
            pushLine("URL: " + url, COL_SYS);
            redrawChatArea(); redrawInput();
        } else if (msg == "setwifi") {
            String ssid = readLine("WiFi SSID:");
            String pass = readLine("WiFi password:", true);
            nvsPutString("wifi_ssid", ssid);
            nvsPutString("wifi_pass", pass);
            tft.fillScreen(COL_BG);
            drawChatTopbar();
            pushLine("Reconnecting...", COL_SYS);
            redrawChatArea(); redrawInput();
            WiFi.disconnect();
            WiFi.begin(ssid.c_str(), pass.c_str());
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(300);
            pushLine(WiFi.isConnected() ? "WiFi: " + WiFi.localIP().toString() : "WiFi failed.", COL_SYS);
            scrollToBottom(); redrawInput();
        } else if (msg.length() > 0) {
            // Check if msg matches a configured assistant slug
            bool isSlug = false;
            for (int i = 0; i < 2; i++) {
                if (s_assist[i].length() > 0 && msg.equalsIgnoreCase(s_assist[i])) {
                    if (!s_aiTarget.equalsIgnoreCase(s_assist[i])) {
                        s_aiTarget = s_assist[i];
                        nvsPutString("ai_target", s_aiTarget);
                        ctxClear();
                        String disp = s_aiTarget; disp.toUpperCase();
                        pushLine(">> " + disp, g_themeColor);
                        drawChatTopbar();
                    } else {
                        String disp = s_aiTarget; disp.toUpperCase();
                        pushLine("Already on " + disp, COL_SYS);
                    }
                    scrollToBottom(); redrawInput();
                    isSlug = true;
                    break;
                }
            }
            if (!isSlug) {
                sendMessage(msg);
                redrawInput();
            }
        }
    } else if (key == 8 || key == 127) {
        if (s_inputBuf.length() > 0) s_inputBuf.remove(s_inputBuf.length() - 1);
        redrawInput();
    } else if (isprint((unsigned char)key) && s_inputBuf.length() < 120) {
        s_inputBuf += key;
        redrawInput();
    }

    delay(20);
    return true;
}

void chatTrackballUp() {
    scrollUp();
}

void chatTrackballDown() {
    scrollDown();
}

void chatExit() {
    s_tft = nullptr;
}
