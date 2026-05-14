#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <WebServer.h>

#include "ui/theme.h"
#include "ui/home.h"
#include "ui/widgets.h"
#include "config/nvs_config.h"
#include "net/wifi_mgr.h"
#include "persona/persona_mgr.h"
#include "modules/chat.h"
#include "modules/weather.h"
#include "modules/solar.h"
#include "modules/btc.h"
#include "modules/sysinfo.h"
#include "modules/noaa.h"
#include "modules/world.h"

// ── Board pins ────────────────────────────────────────────────────────────────
#define BOARD_POWERON    10
#define BOARD_I2C_SDA    18
#define BOARD_I2C_SCL     8
#define BOARD_SPI_SCK    40
#define BOARD_SPI_MISO   38
#define BOARD_SPI_MOSI   41
#define BOARD_TFT_CS     12
#define BOARD_SDCARD_CS  39
#define RADIO_CS_PIN      9
#define BOARD_BL_PIN     42
#define BOARD_TBOX_UP     3
#define BOARD_TBOX_DOWN  15
#define BOARD_TBOX_LEFT   2
#define BOARD_TBOX_RIGHT  1
#define BOARD_TBOX_CLICK  0
#define KB_ADDR         0x55

// ── Backlight ─────────────────────────────────────────────────────────────────
#define BL_PWM_CHANNEL  0
#define BL_PWM_FREQ     1000
#define BL_PWM_BITS     8

static void initBrightness() {
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_BITS);
    ledcAttachPin(BOARD_BL_PIN, BL_PWM_CHANNEL);
    int level16 = nvsGetInt("brightness", 16);
    if (level16 < 1) level16 = 1;
    if (level16 > 16) level16 = 16;
    ledcWrite(BL_PWM_CHANNEL, (uint32_t)level16 * 255 / 16);
}

// ── Screen state ──────────────────────────────────────────────────────────────
enum Screen { SCR_HOME, SCR_CHAT, SCR_WEATHER, SCR_SOLAR, SCR_BTC, SCR_SYSINFO, SCR_ALERTS, SCR_WORLD, SCR_STUB };

static Screen    s_screen = SCR_HOME;
static TFT_eSPI  tft;
static WebServer s_webServer(80);

// ── WiFi screenshot server ────────────────────────────────────────────────────
// GET http://<device-ip>/ss  → downloads screen.bmp (24-bit BMP, ~230 KB).
// Reads rows via tft.readRect() over SPI — takes 2-5 seconds per request.
static void handleScreenshot() {
    const int W = SCREEN_W, H = SCREEN_H;
    const int fileSize = 54 + W * H * 3;

    // BITMAPFILEHEADER + BITMAPINFOHEADER (negative height = top-down)
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize & 0xFF; hdr[3] = (fileSize >> 8) & 0xFF;
    hdr[4] = (fileSize >> 16) & 0xFF; hdr[5] = (fileSize >> 24) & 0xFF;
    hdr[10] = 54;   // pixel data offset
    hdr[14] = 40;   // BITMAPINFOHEADER size
    hdr[18] = W & 0xFF; hdr[19] = (W >> 8) & 0xFF;
    int32_t negH = -H;
    memcpy(&hdr[22], &negH, 4);
    hdr[26] = 1;    // colour planes
    hdr[28] = 24;   // bits per pixel (RGB888)

    s_webServer.setContentLength(fileSize);
    s_webServer.sendHeader("Content-Disposition", "attachment; filename=\"screen.bmp\"");
    s_webServer.send(200, "image/bmp", "");
    s_webServer.sendContent((const char*)hdr, 54);

    uint16_t px[W];
    uint8_t  row[W * 3];
    for (int y = 0; y < H; y++) {
        tft.readRect(0, y, W, 1, px);
        for (int x = 0; x < W; x++) {
            uint16_t c = px[x];
            // TFT readback returns RGB565 with bytes reversed on the T-Deck panel.
            // /ss?raw=1 keeps the original path for quick comparison.
            if (!s_webServer.hasArg("raw")) c = (uint16_t)((c << 8) | (c >> 8));
            row[x * 3 + 0] = (c & 0x1F) << 3;           // B
            row[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;    // G
            row[x * 3 + 2] = ((c >> 11) & 0x1F) << 3;   // R
        }
        s_webServer.sendContent((const char*)row, W * 3);
    }
}

// ── Keyboard read ─────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Trackball — interrupt-based pulse counting ────────────────────────────────
static volatile int  s_tbUp    = 0;
static volatile int  s_tbDown  = 0;
static volatile int  s_tbLeft  = 0;
static volatile int  s_tbRight = 0;
static volatile bool s_tbClick = false;

void IRAM_ATTR isrTbUp()    { s_tbUp++; }
void IRAM_ATTR isrTbDown()  { s_tbDown++; }
void IRAM_ATTR isrTbLeft()  { s_tbLeft++; }
void IRAM_ATTR isrTbRight() { s_tbRight++; }
void IRAM_ATTR isrTbClick() { s_tbClick = true; }

static void drainTrackball(int &up, int &down, int &left, int &right) {
    up = 0;
    down = 0;
    left = 0;
    right = 0;
    noInterrupts();
    up = s_tbUp; s_tbUp = 0;
    down = s_tbDown; s_tbDown = 0;
    left = s_tbLeft; s_tbLeft = 0;
    right = s_tbRight; s_tbRight = 0;
    interrupts();
}

static void handleHomeTrackball() {
    int up, down, left, right;
    drainTrackball(up, down, left, right);
    for (int i = 0; i < up;    i++) homeNavUp(tft);
    for (int i = 0; i < down;  i++) homeNavDown(tft);
    for (int i = 0; i < left;  i++) homeNavRight(tft);
    for (int i = 0; i < right; i++) homeNavLeft(tft);
}

static bool handleScreenTrackball() {
    int up, down, left, right;
    drainTrackball(up, down, left, right);

    if (s_screen == SCR_CHAT) {
        for (int i = 0; i < up;   i++) chatTrackballUp();
        for (int i = 0; i < down; i++) chatTrackballDown();
    } else if (s_screen == SCR_ALERTS) {
        for (int i = 0; i < up;   i++) noaaTrackballUp();
        for (int i = 0; i < down; i++) noaaTrackballDown();
    } else if (s_screen == SCR_WORLD) {
        for (int i = 0; i < up;   i++) worldTrackballUp();
        for (int i = 0; i < down; i++) worldTrackballDown();
    }

    // On module screens, rolling left backs out to the launcher.
    return right > 0;
}

// ── Return to home ────────────────────────────────────────────────────────────
static void returnHome() {
    if (s_screen == SCR_CHAT) chatExit();
    s_screen = SCR_HOME;
    homeInit(tft);
}

// ── Launch a tile ─────────────────────────────────────────────────────────────
static void launchTile(TileID id) {
    switch (id) {
        case TILE_CHAT:
            s_screen = SCR_CHAT;
            chatInit(tft);
            break;
        case TILE_WEATHER:
            s_screen = SCR_WEATHER;
            weatherInit(tft);
            break;
        case TILE_SOLAR:
            s_screen = SCR_SOLAR;
            solarInit(tft);
            break;
        case TILE_BTC:
            s_screen = SCR_BTC;
            btcInit(tft);
            break;
        case TILE_SYSINFO:
            s_screen = SCR_SYSINFO;
            sysinfoInit(tft);
            break;
        case TILE_ALERTS:
            s_screen = SCR_ALERTS;
            noaaInit(tft);
            break;
        case TILE_WORLD:
            s_screen = SCR_WORLD;
            worldInit(tft);
            break;
        case TILE_FIRE:
            s_screen = SCR_WORLD;
            worldInitFires(tft);
            break;
        default:
            s_screen = SCR_STUB;
            tft.fillScreen(COL_BG);
            drawTopbar(tft, ">> AI TERMINAL", "", COL_CYAN);
            tft.setTextFont(FONT_MED);
            tft.setTextColor(COL_GREY_MID, COL_BG);
            tft.drawCentreString("COMING SOON", SCREEN_W / 2, SCREEN_H / 2 - 10, FONT_MED);
            tft.setTextFont(FONT_SMALL);
            tft.setTextColor(COL_GREY_DIM, COL_BG);
            tft.drawCentreString("Press any key to return", SCREEN_W / 2, SCREEN_H / 2 + 14, FONT_SMALL);
            break;
    }
}

// ── SD portal.txt boot load ───────────────────────────────────────────────────
static void loadPortalUrlFromSD() {
    if (!SD.begin(BOARD_SDCARD_CS)) return;
    if (!SD.exists("/portal.txt")) { SD.end(); return; }
    File f = SD.open("/portal.txt", FILE_READ);
    if (!f) { SD.end(); return; }
    String url = f.readStringUntil('\n');
    url.trim();
    // Strip UTF-8 BOM if present (0xEF 0xBB 0xBF)
    if (url.length() >= 3 &&
        (uint8_t)url[0] == 0xEF &&
        (uint8_t)url[1] == 0xBB &&
        (uint8_t)url[2] == 0xBF) url = url.substring(3);
    if (url.endsWith("/")) url.remove(url.length() - 1);
    f.close();
    SD.end();
    if (url.length() > 0) nvsPutString("server_url", url);
}

// ── Splash screen ─────────────────────────────────────────────────────────────
// SD donki.txt boot load
static void loadDonkiKeyFromSD() {
    if (!SD.begin(BOARD_SDCARD_CS)) return;
    if (!SD.exists("/donki.txt")) { SD.end(); return; }
    File f = SD.open("/donki.txt", FILE_READ);
    if (!f) { SD.end(); return; }
    String key = f.readStringUntil('\n');
    key.trim();
    if (key.length() >= 3 &&
        (uint8_t)key[0] == 0xEF &&
        (uint8_t)key[1] == 0xBB &&
        (uint8_t)key[2] == 0xBF) key = key.substring(3);
    f.close();
    if (key.length() > 0) {
        nvsPutString("donki_key", key);
    }
    SD.end();
}
static void showSplash() {
    tft.fillScreen(COL_BG);
    tft.setTextFont(FONT_LARGE);
    tft.setTextColor(COL_CYAN, COL_BG);
    tft.drawCentreString("AI TERMINAL", SCREEN_W / 2, 58, FONT_LARGE);
    tft.setTextFont(FONT_MED);
    tft.setTextColor(COL_CYAN, COL_BG);
    tft.drawCentreString("T-DECK // MayDay", SCREEN_W / 2, 100, FONT_MED);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(COL_GREY_DIM, COL_BG);
    tft.drawCentreString("xXQuantumSmokeXx", SCREEN_W / 2, 124, FONT_SMALL);
    drawCornerBrackets(tft, 6, 44, SCREEN_W - 12, 100, COL_CYAN, 12);
    delay(1800);
    tft.fillScreen(COL_BG);
}

// ── Boot WiFi + NTP ───────────────────────────────────────────────────────────
static void bootWifi() {
    String ssid, pass;
    if (wifiLoadFromSD(ssid, pass)) wifiSaveCreds(ssid, pass);

    tft.fillScreen(COL_BG);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(COL_GREY_MID, COL_BG);
    tft.drawString("Connecting to WiFi...", 4, 10);

    if (wifiConnect()) {
        homeSetWifiStatus(true);
        tft.setTextColor(COL_CYAN, COL_BG);
        tft.drawString("Connected: " + wifiIP(), 4, 26);
        // NTP sync — Eastern time with auto DST
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
        tzset();
        tft.setTextColor(COL_GREY_MID, COL_BG);
        tft.drawString("Syncing time...", 4, 42);
        uint32_t t0 = millis();
        while (time(nullptr) < 1000000 && millis() - t0 < 4000) delay(100);
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            char tbuf[24];
            snprintf(tbuf, sizeof(tbuf), "Time: %02d:%02d ET", ti.tm_hour, ti.tm_min);
            tft.setTextColor(COL_CYAN, COL_BG);
            tft.drawString(tbuf, 4, 56);
        }
        s_webServer.on("/ss", HTTP_GET, handleScreenshot);
        s_webServer.begin();
        delay(600);
    } else {
        tft.setTextColor(COL_AMBER, COL_BG);
        tft.drawString("WiFi failed. Use setwifi in chat.", 4, 26);
        delay(1500);
    }
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(BOARD_POWERON,    OUTPUT); digitalWrite(BOARD_POWERON,    HIGH);
    pinMode(BOARD_SDCARD_CS,  OUTPUT); digitalWrite(BOARD_SDCARD_CS,  HIGH);
    pinMode(RADIO_CS_PIN,     OUTPUT); digitalWrite(RADIO_CS_PIN,     HIGH);
    pinMode(BOARD_TFT_CS,     OUTPUT); digitalWrite(BOARD_TFT_CS,     HIGH);
    pinMode(BOARD_SPI_MISO,   INPUT_PULLUP);
    pinMode(BOARD_TBOX_UP,    INPUT_PULLUP);
    pinMode(BOARD_TBOX_DOWN,  INPUT_PULLUP);
    pinMode(BOARD_TBOX_LEFT,  INPUT_PULLUP);
    pinMode(BOARD_TBOX_RIGHT, INPUT_PULLUP);
    pinMode(BOARD_TBOX_CLICK, INPUT_PULLUP);

    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);
    initBrightness();

    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_UP),    isrTbUp,    FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_DOWN),  isrTbDown,  FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_LEFT),  isrTbLeft,  FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_RIGHT), isrTbRight, FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_CLICK), isrTbClick, FALLING);

    showSplash();
    loadPortalUrlFromSD();
    loadDonkiKeyFromSD();
    bootWifi();
    personaMgrInit();

    homeInit(tft);
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    s_webServer.handleClient();  // non-blocking; serves /ss screenshot on demand

    if (s_screen == SCR_HOME) {
        handleHomeTrackball();

        bool clicked = false;
        noInterrupts();
        if (s_tbClick) { s_tbClick = false; clicked = true; }
        interrupts();
        if (clicked) { launchTile(homeSelected()); return; }

        char key = readKeyboard();
        if      (key == '\r' || key == '\n') { launchTile(homeSelected()); return; }
        else if (key == 'w' || key == 'i')   homeNavUp(tft);
        else if (key == 's' || key == 'k')   homeNavDown(tft);
        else if (key == 'a' || key == 'j')   homeNavLeft(tft);
        else if (key == 'd' || key == 'l')   homeNavRight(tft);

        homeTick(tft);
        delay(20);

    } else if (s_screen == SCR_CHAT) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!chatLoop(tft)) returnHome();

    } else if (s_screen == SCR_WEATHER) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!weatherLoop(tft)) returnHome();

    } else if (s_screen == SCR_SOLAR) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!solarLoop(tft)) returnHome();

    } else if (s_screen == SCR_BTC) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!btcLoop(tft)) returnHome();

    } else if (s_screen == SCR_SYSINFO) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!sysinfoLoop(tft)) returnHome();

    } else if (s_screen == SCR_ALERTS) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!noaaLoop(tft)) returnHome();

    } else if (s_screen == SCR_WORLD) {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (!worldLoop(tft)) returnHome();

    } else {
        if (handleScreenTrackball()) { returnHome(); return; }
        if (readKeyboard() != 0) returnHome();
        delay(20);
    }
}
