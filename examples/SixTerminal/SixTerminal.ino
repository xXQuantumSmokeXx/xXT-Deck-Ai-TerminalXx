/**
 * MayDay Terminal — AI chat client for the LILYGO T-Deck
 * --------------------------------------------------------
 * Supports OpenAI-compatible APIs (OpenAI, Groq, Ollama, etc.)
 * and Anthropic Claude natively.
 *
 * Quick setup via SD card (files self-destruct after read):
 *
 *   wifi.txt     — line 1: SSID,  line 2: password
 *
 *   config.txt   — API settings:
 *     type:  openai              (or: anthropic)
 *     key:   sk-...
 *     url:   https://api.openai.com/v1   (openai only; blank = default)
 *     model: gpt-4o-mini                (blank = default)
 *
 * On-device commands (type and press Enter):
 *   setwifi  — change WiFi credentials
 *   setapi   — reconfigure API (type / key / URL / model)
 *   clear    — clear chat and conversation context
 *
 * Trackball UP scrolls up, DOWN scrolls down through history.
 *
 * OpenAI-compatible spec:
 *   POST {url}/chat/completions
 *   Authorization: Bearer {key}
 *   {"model":"...","messages":[{"role":"user","content":"..."},...]}
 *   Response: {"choices":[{"message":{"content":"..."}}]}
 *
 * Anthropic spec:
 *   POST https://api.anthropic.com/v1/messages
 *   x-api-key: {key}  |  anthropic-version: 2023-06-01
 *   {"model":"...","max_tokens":1024,"messages":[...]}
 *   Response: {"content":[{"type":"text","text":"..."}]}
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <SD.h>
#include <FS.h>

// ── Board pins ────────────────────────────────────────────────────────────────
#define BOARD_POWERON    10
#define BOARD_I2C_SDA    18
#define BOARD_I2C_SCL     8
#define BOARD_BL_PIN     42
#define BOARD_TFT_CS     12
#define BOARD_SDCARD_CS  39
#define RADIO_CS_PIN      9
#define BOARD_SPI_SCK    40
#define BOARD_SPI_MISO   38
#define BOARD_SPI_MOSI   41
#define BOARD_TBOX_UP     3
#define BOARD_TBOX_DOWN  15
#define KB_ADDR        0x55

// ── Display ───────────────────────────────────────────────────────────────────
#define SCREEN_W        320
#define SCREEN_H        240
#define LINE_H           17
#define VISIBLE_LINES    12
#define HISTORY_SIZE    200
#define INPUT_Y        (SCREEN_H - 22)
#define CHARS_PER_LINE   52

// ── Colors ────────────────────────────────────────────────────────────────────
#define COL_BG       TFT_BLACK
#define COL_YOU      0x07FF   // cyan
#define COL_AI       0x07FF   // cyan
#define COL_SYS      0x7BEF   // light grey
#define COL_INPUT_BG 0x1082   // very dark grey
#define COL_INPUT    TFT_WHITE
#define COL_PROMPT   0xFD20   // orange
#define COL_SCROLL   0x4208   // dim grey

// ── Conversation context ──────────────────────────────────────────────────────
#define CTX_MAX_PAIRS   6
#define CTX_SLOTS       (CTX_MAX_PAIRS * 2)
#define CTX_MSG_MAX     500

struct CtxMsg { String role; String content; };
CtxMsg convCtx[CTX_SLOTS];
int    ctxCount = 0;

void ctxAddPair(const String &userMsg, const String &assistantMsg) {
    if (ctxCount >= CTX_SLOTS) {
        for (int i = 0; i < CTX_SLOTS - 2; i++) convCtx[i] = convCtx[i + 2];
        ctxCount = CTX_SLOTS - 2;
    }
    String u = userMsg;      if ((int)u.length() > CTX_MSG_MAX) u = u.substring(0, CTX_MSG_MAX);
    String a = assistantMsg; if ((int)a.length() > CTX_MSG_MAX) a = a.substring(0, CTX_MSG_MAX);
    convCtx[ctxCount++] = { "user",      u };
    convCtx[ctxCount++] = { "assistant", a };
}

void ctxClear() { ctxCount = 0; }

// ── Globals ───────────────────────────────────────────────────────────────────
TFT_eSPI    tft;
Preferences prefs;

String apiType  = "";
String apiKey   = "";
String apiUrl   = "";
String apiModel = "";

// ── Chat history ──────────────────────────────────────────────────────────────
struct ChatLine { String text; uint16_t color; };
ChatLine history[HISTORY_SIZE];
int    historyCount = 0;
int    scrollOffset = 0;
String inputBuf     = "";

// ── Backlight (PWM) ───────────────────────────────────────────────────────────
#define BL_PWM_CHANNEL  0
#define BL_PWM_FREQ     1000
#define BL_PWM_BITS     8
uint8_t brightLevel = 16;   // 1–16

void initBrightness() {
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_BITS);
    ledcAttachPin(BOARD_BL_PIN, BL_PWM_CHANNEL);
    ledcWrite(BL_PWM_CHANNEL, 255);
}

void setBrightness(uint8_t level16) {
    uint32_t duty = (uint32_t)level16 * 255 / 16;
    ledcWrite(BL_PWM_CHANNEL, duty);
}

void brightnessUp() {
    if (brightLevel < 16) brightLevel += 2;
    if (brightLevel > 16) brightLevel = 16;
    setBrightness(brightLevel);
    pushLine("Brightness: " + String(brightLevel) + "/16", COL_SYS);
    scrollToBottom();
}

void brightnessDown() {
    if (brightLevel > 2) brightLevel -= 2;
    setBrightness(brightLevel);
    pushLine("Brightness: " + String(brightLevel) + "/16", COL_SYS);
    scrollToBottom();
}

// ── Rendering ─────────────────────────────────────────────────────────────────
void redrawChat() {
    tft.fillRect(0, 0, SCREEN_W - 4, INPUT_Y - 1, COL_BG);
    tft.setTextFont(1);
    tft.setTextSize(1);
    int lastLine  = historyCount - 1 - scrollOffset;
    int firstLine = lastLine - VISIBLE_LINES + 1;
    for (int i = 0; i < VISIBLE_LINES; i++) {
        int idx = firstLine + i;
        if (idx < 0 || idx >= historyCount) continue;
        tft.setTextColor(history[idx].color, COL_BG);
        tft.drawString(history[idx].text, 2, 2 + i * LINE_H);
    }
    tft.fillRect(SCREEN_W - 4, 0, 4, INPUT_Y - 1, COL_BG);
    if (historyCount > VISIBLE_LINES) {
        int barH   = max(4, (INPUT_Y - 1) * VISIBLE_LINES / historyCount);
        int maxOff = historyCount - VISIBLE_LINES;
        int barY   = (INPUT_Y - 1 - barH) * (maxOff - scrollOffset) / maxOff;
        tft.fillRect(SCREEN_W - 4, barY, 4, barH, COL_SCROLL);
    }
}

void redrawInput() {
    tft.fillRect(0, INPUT_Y, SCREEN_W, SCREEN_H - INPUT_Y, COL_INPUT_BG);
    tft.setTextFont(1);
    tft.setTextSize(1);
    tft.setTextColor(COL_INPUT, COL_INPUT_BG);
    String display = "> " + inputBuf + "_";
    if (display.length() > 40) display = display.substring(display.length() - 40);
    tft.drawString(display, 2, INPUT_Y + 4);
}

void pushLine(const String &text, uint16_t color) {
    if (historyCount < HISTORY_SIZE) {
        history[historyCount++] = { text, color };
    } else {
        for (int i = 0; i < HISTORY_SIZE - 1; i++) history[i] = history[i + 1];
        history[HISTORY_SIZE - 1] = { text, color };
    }
    if (scrollOffset == 0) redrawChat();
}

void pushWrapped(const String &prefix, const String &text, uint16_t color) {
    String full = prefix + text;
    while (full.length() > 0) {
        if ((int)full.length() <= CHARS_PER_LINE) { pushLine(full, color); break; }
        int cut = CHARS_PER_LINE;
        while (cut > 1 && full[cut] != ' ') cut--;
        if (cut <= 1) cut = CHARS_PER_LINE;
        pushLine(full.substring(0, cut), color);
        full = "  " + full.substring(cut + (full[cut] == ' ' ? 1 : 0));
    }
}

void scrollUp()       { int m = max(0, historyCount - VISIBLE_LINES); if (scrollOffset < m) { scrollOffset++; redrawChat(); } }
void scrollDown()     { if (scrollOffset > 0) { scrollOffset--; redrawChat(); } }
void scrollToBottom() { scrollOffset = 0; redrawChat(); }

// ── Keyboard ──────────────────────────────────────────────────────────────────
char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

String readLine(const String &prompt, bool mask = false) {
    String buf = "";
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextColor(COL_PROMPT, COL_BG);
    if ((int)prompt.length() > CHARS_PER_LINE) {
        tft.drawString(prompt.substring(0, CHARS_PER_LINE), 2, 10);
        tft.drawString(prompt.substring(CHARS_PER_LINE), 2, 26);
    } else {
        tft.drawString(prompt, 2, 10);
    }

    auto redraw = [&]() {
        tft.fillRect(0, 42, SCREEN_W, 20, COL_BG);
        tft.setTextColor(COL_INPUT, COL_BG);
        String masked = "";
        if (mask) for (int i = 0; i < (int)buf.length(); i++) masked += '*';
        String show = (mask ? masked : buf) + "_";
        if ((int)show.length() > 38) show = show.substring(show.length() - 38);
        tft.drawString(show, 2, 44);
    };
    redraw();

    while (true) {
        char key = readKeyboard();
        if (key == 0) { delay(20); continue; }
        if (key == '\r' || key == '\n') break;
        if ((key == 8 || key == 127) && buf.length() > 0) buf.remove(buf.length() - 1);
        else if (isprint((unsigned char)key) && buf.length() < 80) buf += key;
        redraw();
        delay(20);
    }
    return buf;
}

// ── NVS ───────────────────────────────────────────────────────────────────────
void saveWiFiCreds(const String &ssid, const String &pass) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
}

bool loadWiFiCreds(String &ssid, String &pass) {
    prefs.begin("wifi", true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
    return ssid.length() > 0;
}

void saveApiConfig() {
    prefs.begin("cfg", false);
    prefs.putString("type",  apiType);
    prefs.putString("key",   apiKey);
    prefs.putString("url",   apiUrl);
    prefs.putString("model", apiModel);
    prefs.end();
}

bool loadApiConfig() {
    prefs.begin("cfg", true);
    apiType  = prefs.getString("type",  "");
    apiKey   = prefs.getString("key",   "");
    apiUrl   = prefs.getString("url",   "");
    apiModel = prefs.getString("model", "");
    prefs.end();
    return apiType.length() > 0 && apiKey.length() > 0;
}

// ── SD card loaders ───────────────────────────────────────────────────────────
bool loadWiFiFromSD(String &ssid, String &pass) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/wifi.txt")) { SD.end(); return false; }
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) { SD.end(); return false; }
    ssid = f.readStringUntil('\n'); ssid.trim();
    pass = f.readStringUntil('\n'); pass.trim();
    f.close();
    SD.end();
    return ssid.length() > 0;
}

bool loadConfigFromSD() {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/config.txt")) { SD.end(); return false; }
    File f = SD.open("/config.txt", FILE_READ);
    if (!f) { SD.end(); return false; }
    bool first = true;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        // Strip UTF-8 BOM if present on first line
        if (first && line.length() >= 3 &&
            (uint8_t)line[0] == 0xEF &&
            (uint8_t)line[1] == 0xBB &&
            (uint8_t)line[2] == 0xBF) {
            line = line.substring(3);
        }
        first = false;
        line.trim();
        int colon = line.indexOf(':');
        if (colon < 0) continue;
        String k = line.substring(0, colon);  k.trim(); k.toLowerCase();
        String v = line.substring(colon + 1); v.trim();
        if (k == "type")  apiType  = v;
        if (k == "key")   apiKey   = v;
        if (k == "url")   apiUrl   = v;
        if (k == "model") apiModel = v;
    }
    f.close();
    if (apiType == "openai") {
        if (apiUrl.length()   == 0) apiUrl   = "https://api.openai.com/v1";
        if (apiModel.length() == 0) apiModel = "gpt-4o-mini";
    } else if (apiType == "anthropic") {
        apiUrl = "https://api.anthropic.com";
        if (apiModel.length() == 0) apiModel = "claude-3-5-haiku-20241022";
    }
    SD.end();
    return apiType.length() > 0 && apiKey.length() > 0;
}

// ── Setup flows ───────────────────────────────────────────────────────────────
void setupWiFiCredentials() {
    String ssid, pass;
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextColor(COL_PROMPT, COL_BG);
    tft.drawString("Checking SD for wifi.txt...", 2, 10);
    delay(1000);
    if (loadWiFiFromSD(ssid, pass)) {
        tft.fillRect(0, 30, SCREEN_W, 20, COL_BG);
        tft.setTextColor(COL_AI, COL_BG);
        tft.drawString("Loaded from SD: " + ssid, 2, 32);
        delay(1500);
        saveWiFiCreds(ssid, pass);
    } else {
        ssid = readLine("Enter WiFi SSID:");
        pass = readLine("Enter WiFi Password:", true);
        saveWiFiCreds(ssid, pass);
    }
    tft.fillScreen(COL_BG);
}

void setupApiConfig() {
    String type = readLine("API type: openai or anthropic");
    type.trim(); type.toLowerCase();
    if (type != "openai" && type != "anthropic") type = "openai";

    String key = readLine("API key:", true);

    String url, model;
    if (type == "openai") {
        url = readLine("Base URL (blank=OpenAI default):");
        url.trim();
        if (url.endsWith("/")) url.remove(url.length() - 1);
        if (url.length() == 0) url = "https://api.openai.com/v1";
        model = readLine("Model (blank=gpt-4o-mini):");
        model.trim();
        if (model.length() == 0) model = "gpt-4o-mini";
    } else {
        url   = "https://api.anthropic.com";
        model = readLine("Model (blank=claude-3-5-haiku-20241022):");
        model.trim();
        if (model.length() == 0) model = "claude-3-5-haiku-20241022";
    }

    apiType = type; apiKey = key; apiUrl = url; apiModel = model;
    saveApiConfig();
    tft.fillScreen(COL_BG);
}

bool connectWiFi() {
    String ssid, pass;
    if (!loadWiFiCreds(ssid, pass)) {
        setupWiFiCredentials();
        loadWiFiCreds(ssid, pass);
    }
    pushLine("Connecting: " + ssid, COL_SYS);
    redrawInput();
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(500);
    return WiFi.status() == WL_CONNECTED;
}

// ── HTTP POST helper ──────────────────────────────────────────────────────────
int doPost(const String &endpoint, const String &body,
           const String &authHeader, const String &authValue,
           const String &extraHeader, const String &extraValue,
           String &responseOut) {
    bool https = endpoint.startsWith("https://");
    int  code  = -1;
    if (https) {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, endpoint);
        http.addHeader("Content-Type", "application/json");
        http.addHeader(authHeader, authValue);
        if (extraHeader.length() > 0) http.addHeader(extraHeader, extraValue);
        http.addHeader("ngrok-skip-browser-warning", "true");
        http.setTimeout(60000);
        code = http.POST(body);
        responseOut = http.getString();
        http.end();
    } else {
        HTTPClient http;
        http.begin(endpoint);
        http.addHeader("Content-Type", "application/json");
        http.addHeader(authHeader, authValue);
        if (extraHeader.length() > 0) http.addHeader(extraHeader, extraValue);
        http.setTimeout(60000);
        code = http.POST(body);
        responseOut = http.getString();
        http.end();
    }
    return code;
}

// ── Send message ──────────────────────────────────────────────────────────────
void sendMessage(const String &msg) {
    pushWrapped("YOU: ", msg, COL_YOU);
    pushLine("...", COL_SYS);
    scrollToBottom();
    redrawInput();

    if (WiFi.status() != WL_CONNECTED) {
        if (historyCount > 0 && history[historyCount-1].text == "...") historyCount--;
        pushLine("ERR: WiFi not connected", COL_SYS);
        scrollToBottom();
        return;
    }
    if (apiKey.length() == 0 || apiType.length() == 0) {
        if (historyCount > 0 && history[historyCount-1].text == "...") historyCount--;
        pushLine("ERR: No API config. Type setapi", COL_SYS);
        scrollToBottom();
        return;
    }

    // Build request
    JsonDocument reqDoc;
    reqDoc["model"] = apiModel;
    if (apiType == "anthropic") reqDoc["max_tokens"] = 1024;
    JsonArray messages = reqDoc["messages"].to<JsonArray>();
    for (int i = 0; i < ctxCount; i++) {
        JsonObject m = messages.add<JsonObject>();
        m["role"]    = convCtx[i].role;
        m["content"] = convCtx[i].content;
    }
    JsonObject newMsg = messages.add<JsonObject>();
    newMsg["role"]    = "user";
    newMsg["content"] = msg;

    String body;
    serializeJson(reqDoc, body);

    // Endpoint and auth
    String endpoint, authHeader, authValue, extraHeader, extraValue;
    if (apiType == "anthropic") {
        endpoint    = "https://api.anthropic.com/v1/messages";
        authHeader  = "x-api-key";
        authValue   = apiKey;
        extraHeader = "anthropic-version";
        extraValue  = "2023-06-01";
    } else {
        endpoint   = apiUrl + "/chat/completions";
        authHeader = "Authorization";
        authValue  = "Bearer " + apiKey;
    }

    String raw;
    int code = doPost(endpoint, body, authHeader, authValue, extraHeader, extraValue, raw);

    if (historyCount > 0 && history[historyCount-1].text == "...") historyCount--;

    if (code == 200) {
        JsonDocument resDoc;
        DeserializationError err = deserializeJson(resDoc, raw);
        if (err) {
            pushLine("ERR: bad JSON", COL_SYS);
        } else {
            String reply;
            if (apiType == "anthropic") {
                reply = resDoc["content"][0]["text"].as<String>();
            } else {
                reply = resDoc["choices"][0]["message"]["content"].as<String>();
            }
            if (reply.length() == 0) {
                pushLine("ERR: empty response", COL_SYS);
            } else {
                ctxAddPair(msg, reply);
                pushWrapped("AI: ", reply, COL_AI);
            }
        }
    } else if (code > 0) {
        pushLine("ERR: HTTP " + String(code), COL_SYS);
        if (raw.length() > 0) pushWrapped("  ", raw.substring(0, 80), COL_SYS);
    } else {
        pushLine("ERR: connection failed (" + String(code) + ")", COL_SYS);
    }

    scrollToBottom();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(BOARD_POWERON,   OUTPUT); digitalWrite(BOARD_POWERON,   HIGH);
    pinMode(BOARD_SDCARD_CS, OUTPUT); digitalWrite(BOARD_SDCARD_CS, HIGH);
    pinMode(RADIO_CS_PIN,    OUTPUT); digitalWrite(RADIO_CS_PIN,    HIGH);
    pinMode(BOARD_TFT_CS,    OUTPUT); digitalWrite(BOARD_TFT_CS,    HIGH);
    pinMode(BOARD_SPI_MISO,  INPUT_PULLUP);
    pinMode(BOARD_TBOX_UP,   INPUT_PULLUP);
    pinMode(BOARD_TBOX_DOWN, INPUT_PULLUP);

    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);
    initBrightness();

    // Splash
    tft.setTextFont(2);
    tft.setTextColor(COL_AI, COL_BG);
    tft.drawCentreString("MAYDAY TERMINAL", SCREEN_W / 2, 75, 2);
    tft.setTextFont(1);
    tft.setTextColor(COL_SYS, COL_BG);
    tft.drawCentreString("AI Chat for T-Deck", SCREEN_W / 2, 105, 1);
    tft.drawCentreString("github.com/xXQuantumSmokeXx", SCREEN_W / 2, 120, 1);
    delay(2500);
    tft.fillScreen(COL_BG);

    tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
    redrawInput();

    // WiFi
    if (connectWiFi()) {
        pushLine("WiFi: " + WiFi.localIP().toString(), COL_SYS);
    } else {
        pushLine("WiFi failed. Type setwifi.", COL_SYS);
    }

    // API config — SD first, then NVS, then prompt
    pushLine("Checking SD for config.txt...", COL_SYS);
    redrawChat();
    if (loadConfigFromSD()) {
        saveApiConfig();
        pushLine("API loaded from SD: " + apiType, COL_SYS);
    } else if (!loadApiConfig()) {
        pushLine("No API config found.", COL_SYS);
        redrawChat();
        setupApiConfig();
        tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
        pushLine("API saved: " + apiType + " / " + apiModel, COL_SYS);
    } else {
        pushLine("API: " + apiType + " / " + apiModel, COL_SYS);
    }

    pushLine("setwifi  setapi  clear  b+  b-", COL_SYS);
    redrawChat();
    redrawInput();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    static bool lastUp = HIGH, lastDown = HIGH;
    bool curUp   = digitalRead(BOARD_TBOX_UP);
    bool curDown = digitalRead(BOARD_TBOX_DOWN);
    if (curUp   != lastUp   && curUp   == LOW) scrollUp();
    if (curDown != lastDown && curDown == LOW) scrollDown();
    lastUp   = curUp;
    lastDown = curDown;

    char key = readKeyboard();
    if (key == 0) { delay(20); return; }

    if (key == '\r' || key == '\n') {
        String msg = inputBuf;
        msg.trim();
        inputBuf = "";
        redrawInput();

        if (msg == "b+") {
            brightnessUp();
            redrawChat();
            redrawInput();
        } else if (msg == "b-") {
            brightnessDown();
            redrawChat();
            redrawInput();
        } else if (msg == "setwifi") {
            setupWiFiCredentials();
            tft.fillScreen(COL_BG);
            tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
            historyCount = 0; scrollOffset = 0;
            WiFi.disconnect();
            pushLine(connectWiFi() ? "WiFi: " + WiFi.localIP().toString() : "WiFi failed.", COL_SYS);
            redrawChat();
            redrawInput();
        } else if (msg == "setapi") {
            setupApiConfig();
            tft.fillScreen(COL_BG);
            tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
            ctxClear();
            pushLine("API: " + apiType + " / " + apiModel, COL_SYS);
            redrawChat();
            redrawInput();
        } else if (msg == "clear") {
            historyCount = 0; scrollOffset = 0;
            ctxClear();
            redrawChat();
            redrawInput();
        } else if (msg.length() > 0) {
            sendMessage(msg);
            redrawInput();
        }
    } else if (key == 8 || key == 127) {
        if (inputBuf.length() > 0) inputBuf.remove(inputBuf.length() - 1);
        redrawInput();
    } else if (isprint((unsigned char)key) && inputBuf.length() < 120) {
        inputBuf += key;
        redrawInput();
    }

    delay(20);
}
