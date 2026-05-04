/**
 * MayDay Terminal — AI chat client for the LILYGO T-Deck
 * --------------------------------------------------------
 * On first boot you will be prompted to enter:
 *   1. WiFi SSID and password
 *   2. Your server URL (e.g. https://your-ngrok-url.ngrok-free.app)
 *
 * Commands (type and press Enter):
 *   setwifi  — change WiFi credentials
 *   seturl   — change server URL
 *   six      — talk to Six (purple)
 *   nova     — talk to Nova (red)
 *
 * Trackball UP/DOWN scrolls chat history.
 *
 * Backend spec: POST {"message":"..."} → {"response":"..."}
 * Compatible with MayDay Portal or any server implementing /simple
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
#define COL_SIX      0xC81F   // purple
#define COL_NOVA     0xF800   // red
#define COL_SYS      0x7BEF   // light grey
#define COL_INPUT_BG 0x1082   // very dark grey
#define COL_INPUT    TFT_WHITE
#define COL_PROMPT   0xFD20   // orange
#define COL_SCROLL   0x4208   // dim grey

TFT_eSPI    tft;
Preferences prefs;
String      activeAgent = "six";
String      serverBase  = "";   // loaded from NVS at boot

// ── Chat history ──────────────────────────────────────────────────────────────
struct ChatLine { String text; uint16_t color; };
ChatLine history[HISTORY_SIZE];
int    historyCount = 0;
int    scrollOffset = 0;
String inputBuf     = "";

// ── Backlight ─────────────────────────────────────────────────────────────────
void setBrightness(uint8_t val) {
    static uint8_t level = 0;
    const  uint8_t steps = 16;
    if (val == 0) { digitalWrite(BOARD_BL_PIN, 0); delay(3); level = 0; return; }
    if (level == 0) { digitalWrite(BOARD_BL_PIN, 1); level = steps; delayMicroseconds(30); }
    int num = (steps + (steps - val) - (steps - level)) % steps;
    for (int i = 0; i < num; i++) { digitalWrite(BOARD_BL_PIN, 0); digitalWrite(BOARD_BL_PIN, 1); }
    level = val;
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
        int barH  = max(4, (INPUT_Y - 1) * VISIBLE_LINES / historyCount);
        int maxOff = historyCount - VISIBLE_LINES;
        int barY  = (INPUT_Y - 1 - barH) * (maxOff - scrollOffset) / maxOff;
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

// Blocking single-line input shown on a clean screen
String readLine(const String &prompt, bool mask = false) {
    String buf = "";
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextColor(COL_PROMPT, COL_BG);
    // Word-wrap the prompt if it's long
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
        tft.drawString((mask ? masked : buf) + "_", 2, 44);
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

// ── NVS helpers ───────────────────────────────────────────────────────────────
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

void saveServerUrl(const String &url) {
    prefs.begin("cfg", false);
    prefs.putString("url", url);
    prefs.end();
}

bool loadServerUrl(String &url) {
    prefs.begin("cfg", true);
    url = prefs.getString("url", "");
    prefs.end();
    return url.length() > 0;
}

// ── SD card credential loader ─────────────────────────────────────────────────
bool loadCredsFromSD(String &ssid, String &pass) {
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists("/wifi.txt")) { SD.end(); return false; }
    File f = SD.open("/wifi.txt", FILE_READ);
    if (!f) { SD.end(); return false; }
    ssid = f.readStringUntil('\n'); ssid.trim();
    pass = f.readStringUntil('\n'); pass.trim();
    f.close();
    SD.remove("/wifi.txt");
    SD.end();
    return ssid.length() > 0;
}

// ── Setup flows ───────────────────────────────────────────────────────────────
void setupWiFiCredentials() {
    String ssid, pass;
    tft.fillScreen(COL_BG);
    tft.setTextFont(1);
    tft.setTextColor(COL_PROMPT, COL_BG);
    tft.drawString("Checking SD for wifi.txt...", 2, 10);
    delay(1000);

    if (loadCredsFromSD(ssid, pass)) {
        tft.fillRect(0, 30, SCREEN_W, 20, COL_BG);
        tft.setTextColor(COL_SIX, COL_BG);
        tft.drawString("Loaded from SD: " + ssid, 2, 32);
        delay(1500);
        saveWiFiCreds(ssid, pass);
    } else {
        ssid = readLine("Enter WiFi SSID:");
        pass = readLine("Enter WiFi Password:");
        saveWiFiCreds(ssid, pass);
    }
    tft.fillScreen(COL_BG);
}

void setupServerUrl() {
    String url = readLine("Enter server URL:\n(e.g. https://abc123.ngrok-free.app)");
    // Strip trailing slash
    url.trim();
    if (url.endsWith("/")) url.remove(url.length() - 1);
    saveServerUrl(url);
    serverBase = url;
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

// ── HTTP request (auto HTTP/HTTPS) ────────────────────────────────────────────
void sendMessage(const String &msg) {
    pushWrapped("YOU: ", msg, COL_YOU);
    pushLine("...", COL_SYS);
    scrollToBottom();
    redrawInput();

    if (WiFi.status() != WL_CONNECTED) {
        history[historyCount > 0 ? historyCount - 1 : 0] = { "ERR: WiFi not connected", COL_SYS };
        redrawChat();
        return;
    }

    if (serverBase.length() == 0) {
        if (historyCount > 0 && history[historyCount-1].text == "...") historyCount--;
        pushLine("ERR: No URL set. Type seturl", COL_SYS);
        scrollToBottom();
        return;
    }

    String endpoint = serverBase + "/" + activeAgent + "/simple";
    bool   useHttps = serverBase.startsWith("https://");

    StaticJsonDocument<256> req;
    req["message"] = msg;
    String body;
    serializeJson(req, body);

    int code = -1;
    String raw = "";

    if (useHttps) {
        WiFiClientSecure client;
        client.setInsecure();
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

    if (historyCount > 0 && history[historyCount - 1].text == "...") historyCount--;

    if (code == 200) {
        StaticJsonDocument<4096> res;
        if (deserializeJson(res, raw)) {
            pushLine("ERR: bad JSON", COL_SYS);
        } else {
            String   label = activeAgent == "nova" ? "NOVA: " : "SIX: ";
            uint16_t col   = activeAgent == "nova" ? COL_NOVA : COL_SIX;
            pushWrapped(label, res["response"].as<String>(), col);
        }
    } else if (code > 0) {
        pushLine("ERR: HTTP " + String(code), COL_SYS);
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
    pinMode(BOARD_BL_PIN, OUTPUT);
    setBrightness(16);

    // Splash
    tft.setTextFont(2);
    tft.setTextColor(COL_SIX, COL_BG);
    tft.drawCentreString("MAYDAY TERMINAL", SCREEN_W / 2, 75, 2);
    tft.setTextFont(1);
    tft.setTextColor(COL_SYS, COL_BG);
    tft.drawCentreString("AI Chat for T-Deck", SCREEN_W / 2, 105, 1);
    tft.drawCentreString("github.com/xXQuantumSmokeXx", SCREEN_W / 2, 120, 1);
    delay(1200);
    tft.fillScreen(COL_BG);

    tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
    redrawInput();

    // Connect WiFi
    if (connectWiFi()) {
        pushLine("WiFi: " + WiFi.localIP().toString(), COL_SYS);
    } else {
        pushLine("WiFi failed. Type setwifi.", COL_SYS);
    }

    // Load or prompt for server URL
    if (!loadServerUrl(serverBase)) {
        pushLine("No server URL set.", COL_SYS);
        redrawChat();
        setupServerUrl();
        tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
        pushLine("URL saved: " + serverBase, COL_SYS);
    } else {
        pushLine("Server: " + serverBase, COL_SYS);
    }

    pushLine("six/nova=switch  setwifi  seturl", COL_SYS);
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

        if (msg == "six" || msg == "nova") {
            activeAgent = msg;
            String label = activeAgent == "nova" ? "NOVA: red" : "SIX: purple";
            pushLine("Switched to " + label, COL_SYS);
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
        } else if (msg == "seturl") {
            setupServerUrl();
            tft.fillScreen(COL_BG);
            tft.drawFastHLine(0, INPUT_Y - 1, SCREEN_W, COL_SYS);
            pushLine("URL: " + serverBase, COL_SYS);
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
