#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <TouchLib.h>

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
#include "modules/waze.h"
#include "modules/news.h"
#include "modules/shtf.h"
#include "modules/sigil.h"

// ── Touch ─────────────────────────────────────────────────────────────────────
#define TOUCH_INT_PIN   16

static TouchLib *s_touch      = nullptr;
static bool      s_touchReady  = false;
static bool      s_prevTouched = false;
static uint16_t  s_tapX        = 0;
static uint16_t  s_tapY        = 0;
static uint32_t  s_lastTapMs   = 0;
static uint32_t  s_touchDownMs = 0;

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
enum Screen { SCR_HOME, SCR_CHAT, SCR_WEATHER, SCR_SOLAR, SCR_BTC, SCR_SYSINFO, SCR_ALERTS, SCR_WORLD, SCR_HAZARD, SCR_ORACLE, SCR_ROAD, SCR_SHTF, SCR_STUB };

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

#define TB_DEBOUNCE_MS 12
static volatile uint32_t s_tbLastUp    = 0;
static volatile uint32_t s_tbLastDown  = 0;
static volatile uint32_t s_tbLastLeft  = 0;
static volatile uint32_t s_tbLastRight = 0;
static volatile uint32_t s_tbLastClick = 0;

void IRAM_ATTR isrTbUp()    { uint32_t n = millis(); if (n - s_tbLastUp    > TB_DEBOUNCE_MS) { s_tbLastUp    = n; s_tbUp++;    } }
void IRAM_ATTR isrTbDown()  { uint32_t n = millis(); if (n - s_tbLastDown  > TB_DEBOUNCE_MS) { s_tbLastDown  = n; s_tbDown++;  } }
void IRAM_ATTR isrTbLeft()  { uint32_t n = millis(); if (n - s_tbLastLeft  > TB_DEBOUNCE_MS) { s_tbLastLeft  = n; s_tbLeft++;  } }
void IRAM_ATTR isrTbRight() { uint32_t n = millis(); if (n - s_tbLastRight > TB_DEBOUNCE_MS) { s_tbLastRight = n; s_tbRight++; } }
void IRAM_ATTR isrTbClick() { uint32_t n = millis(); if (n - s_tbLastClick > TB_DEBOUNCE_MS) { s_tbLastClick = n; s_tbClick = true; } }

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
    // Cap at 1 per tick — prevents burst redraws when trackball generates multiple pulses
    if (up)    homeNavUp(tft);
    if (down)  homeNavDown(tft);
    if (left)  homeNavRight(tft);
    if (right) homeNavLeft(tft);
}

static void handleScreenTrackball() {
    int up, down, left, right;
    drainTrackball(up, down, left, right);

    if (s_screen == SCR_CHAT) {
        if (up)   chatTrackballUp();
        if (down) chatTrackballDown();
    } else if (s_screen == SCR_ALERTS) {
        if (up)   noaaTrackballUp();
        if (down) noaaTrackballDown();
    } else if (s_screen == SCR_WORLD) {
        if (up)   worldTrackballUp();
        if (down) worldTrackballDown();
    } else if (s_screen == SCR_HAZARD) {
        if (up)   wazeTrackballUp();
        if (down) wazeTrackballDown();
    } else if (s_screen == SCR_ORACLE) {
        if (up)    sigilTrackballUp();
        if (down)  sigilTrackballDown();
        if (left)  sigilTrackballRight();  // T-Deck physical left = logical right
        if (right) sigilTrackballLeft();
    } else if (s_screen == SCR_ROAD) {
        if (up)   newsTrackballUp();
        if (down) newsTrackballDown();
    } else if (s_screen == SCR_SHTF) {
        if (up)   shtfTrackballUp();
        if (down) shtfTrackballDown();
    }
}

static void launchTile(TileID id);  // forward declaration

// ── Touch init & home-screen tap handler ─────────────────────────────────────
static void initTouch() {
    pinMode(TOUCH_INT_PIN, INPUT_PULLUP);
    delay(200);  // GT911 needs time to settle after power-on

    // GT911 address depends on INT pin state at reset: probe both
    const uint8_t candidates[] = {0x5D, 0x14};
    uint8_t foundAddr = 0;
    for (int i = 0; i < 2; i++) {
        Wire.beginTransmission(candidates[i]);
        if (Wire.endTransmission() == 0) { foundAddr = candidates[i]; break; }
        delay(20);
    }

    if (foundAddr == 0) {
        Serial.println("[Touch] GT911 not found on I2C");
        return;
    }

    Serial.printf("[Touch] GT911 at 0x%02X\n", foundAddr);
    s_touch = new TouchLib(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, foundAddr);
    s_touch->init();  // software reset; leaves interrupt mode at NVM default

    // Set interrupt mode to falling-edge (0x01) with proper CONFIG_FRESH.
    // Without CONFIG_FRESH the GT911 ignores the register write entirely.
    // Wire receive buffer is ~128 bytes so read the 184-byte config in two chunks.
    {
        uint8_t cfg[184];

        Wire.beginTransmission(foundAddr);
        Wire.write(0x80); Wire.write(0x47);        // GT911_CONFIG_START
        Wire.endTransmission(false);
        Wire.requestFrom(foundAddr, (uint8_t)128);
        for (int i = 0; i <  128; i++) cfg[i]       = Wire.available() ? Wire.read() : 0;

        Wire.beginTransmission(foundAddr);
        Wire.write(0x80); Wire.write(0xC7);        // 0x8047 + 128
        Wire.endTransmission(false);
        Wire.requestFrom(foundAddr, (uint8_t)56);
        for (int i = 0; i <   56; i++) cfg[128 + i] = Wire.available() ? Wire.read() : 0;

        // MODULE_SWITCH_1 is at 0x804D = offset 6 from 0x8047
        cfg[6] = (cfg[6] & 0xFC) | 0x01;           // falling-edge mode

        // Write patched register
        Wire.beginTransmission(foundAddr);
        Wire.write(0x80); Wire.write(0x4D);
        Wire.write(cfg[6]);
        Wire.endTransmission();

        // Recompute checksum (two's complement of byte-sum of cfg[0..183])
        uint8_t sum = 0;
        for (int i = 0; i < 184; i++) sum += cfg[i];
        uint8_t chk = (~sum) + 1;

        Wire.beginTransmission(foundAddr);
        Wire.write(0x80); Wire.write(0xFF);        // GT911_CONFIG_CHKSUM
        Wire.write(chk);
        Wire.endTransmission();

        Wire.beginTransmission(foundAddr);
        Wire.write(0x81); Wire.write(0x00);        // GT911_CONFIG_FRESH
        Wire.write((uint8_t)0x01);
        Wire.endTransmission();

        delay(100);
        Serial.printf("[Touch] INT mode=falling-edge chk=0x%02X\n", chk);
    }

    s_touch->setRotation(1);
    s_touchReady = true;
    Serial.println("[Touch] ready");
}

// Flush GT911 state when (re-)entering home screen.
static void touchDrain() {
    if (s_touch && s_touchReady) s_touch->read();  // consume any pending data
    s_prevTouched = false;
    s_tapX = 0;
    s_tapY = 0;
    s_touchDownMs = 0;
    s_lastTapMs = millis();  // 300 ms block against ghost taps on re-entry
}

static void handleHomeTouch() {
    if (!s_touch || !s_touchReady) return;

    // Safety: un-stick prevTouched if GT911 misses its lift report (~1 s timeout)
    if (s_prevTouched && s_touchDownMs && (millis() - s_touchDownMs > 1000)) {
        s_prevTouched = false;
        s_touchDownMs = 0;
    }

    // GT911 in falling-edge mode: INT is held LOW while unread data exists.
    // Only call read() when INT is LOW — prevents spurious 0x00 sync writes
    // that confuse the chip between events.
    if (digitalRead(TOUCH_INT_PIN) != LOW) return;

    bool touched = s_touch->read() && s_touch->getPointNum() > 0;

    if (touched) {
        TP_Point p = s_touch->getPoint(0);
        if (p.x < SCREEN_W && p.y < SCREEN_H) {
            s_tapX = p.x;
            s_tapY = (SCREEN_H - 1) - p.y;  // GT911 Y is mirrored on T-Deck
            s_touchDownMs = millis();
        } else {
            touched = false;
        }
    }

    // Fire on finger-lift (GT911 sends 0-point report on release → read() returns false)
    if (!touched && s_prevTouched) {
        s_touchDownMs = 0;
        Serial.printf("[Touch] tap x=%d y=%d\n", s_tapX, s_tapY);
        if (s_tapY >= CONTENT_Y && s_tapY < (uint16_t)(SCREEN_H - BOTTOMBAR_H)) {
            int col = s_tapX / TILE_W;
            int row = (s_tapY - CONTENT_Y) / TILE_H;
            if (col < TILE_COLS && row < TILE_ROWS) {
                uint32_t now = millis();
                if (now - s_lastTapMs > 300) {
                    s_lastTapMs = now;
                    s_prevTouched = false;
                    launchTile((TileID)(row * TILE_COLS + col));
                    return;
                }
            }
        }
    }

    s_prevTouched = touched;
}

// ── Module touch handler (SCR_ALERTS etc.) ────────────────────────────────────
static void handleModuleTouch() {
    if (!s_touch || !s_touchReady) return;

    if (s_prevTouched && s_touchDownMs && (millis() - s_touchDownMs > 1000)) {
        s_prevTouched = false;
        s_touchDownMs = 0;
    }

    if (digitalRead(TOUCH_INT_PIN) != LOW) return;

    bool touched = s_touch->read() && s_touch->getPointNum() > 0;

    if (touched) {
        TP_Point p = s_touch->getPoint(0);
        if (p.x < SCREEN_W && p.y < SCREEN_H) {
            s_tapX = p.x;
            s_tapY = (SCREEN_H - 1) - p.y;
            s_touchDownMs = millis();
        } else {
            touched = false;
        }
    }

    if (!touched && s_prevTouched) {
        s_touchDownMs = 0;
        uint32_t now = millis();
        if (now - s_lastTapMs > 300) {
            s_lastTapMs = now;
            s_prevTouched = false;
            if      (s_screen == SCR_ALERTS) noaaTouchTap(s_tapX, s_tapY);
            else if (s_screen == SCR_ORACLE) sigilTouchTap(s_tapX, s_tapY);
            return;
        }
    }

    s_prevTouched = touched;
}

// ── Return to home ────────────────────────────────────────────────────────────
static void returnHome() {
    if (s_screen == SCR_CHAT) chatExit();
    s_screen = SCR_HOME;
    homeInit(tft);
    touchDrain();
}

// ── Launch a tile ─────────────────────────────────────────────────────────────
static void launchTile(TileID id) {
    switch (id) {
        case TILE_CHAT: {
            s_screen = SCR_CHAT;
            chatInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_WEATHER: {
            s_screen = SCR_WEATHER;
            weatherInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_SOLAR: {
            s_screen = SCR_SOLAR;
            solarInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_BTC: {
            s_screen = SCR_BTC;
            btcInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_SYSINFO: {
            s_screen = SCR_SYSINFO;
            sysinfoInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_ALERTS: {
            s_screen = SCR_ALERTS;
            noaaInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during init
            break;
        }
        case TILE_WORLD: {
            s_screen = SCR_WORLD;
            worldInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);  // discard pulses stacked during fetch
            break;
        }
        case TILE_FIRE: {
            s_screen = SCR_WORLD;
            worldInitFires(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);
            break;
        }
        case TILE_HAZARD: {
            s_screen = SCR_HAZARD;
            wazeHazardInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);
            break;
        }
        case TILE_ORACLE: {
            s_screen = SCR_ORACLE;
            sigilInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);
            break;
        }
        case TILE_ROAD: {
            s_screen = SCR_ROAD;
            newsInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);
            break;
        }
        case TILE_SHTF: {
            s_screen = SCR_SHTF;
            shtfInit(tft);
            int _u, _d, _l, _r;
            drainTrackball(_u, _d, _l, _r);
            break;
        }
        default:
            s_screen = SCR_STUB;
            tft.fillScreen(COL_BG);
            drawTopbar(tft, ">> AI TERMINAL", "", g_themeColor);
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
static void loadSdKey(const char *filename, const char *nvsKey) {
    if (!SD.begin(BOARD_SDCARD_CS)) return;
    if (!SD.exists(filename)) { SD.end(); return; }
    File f = SD.open(filename, FILE_READ);
    if (!f) { SD.end(); return; }
    String key = f.readStringUntil('\n');
    key.trim();
    if (key.length() >= 3 &&
        (uint8_t)key[0] == 0xEF &&
        (uint8_t)key[1] == 0xBB &&
        (uint8_t)key[2] == 0xBF) key = key.substring(3);
    f.close();
    SD.end();
    if (key.length() > 0) nvsPutString(nvsKey, key);
}

static void loadWazeKeyFromSD() {
    if (!SD.begin(BOARD_SDCARD_CS)) return;
    if (!SD.exists("/waze.txt")) { SD.end(); return; }
    File f = SD.open("/waze.txt", FILE_READ);
    if (!f) { SD.end(); return; }
    String key = f.readStringUntil('\n');
    key.trim();
    if (key.length() >= 3 &&
        (uint8_t)key[0] == 0xEF &&
        (uint8_t)key[1] == 0xBB &&
        (uint8_t)key[2] == 0xBF) key = key.substring(3);
    f.close();
    if (key.length() > 0) nvsPutString("waze_key", key);
    SD.end();
}
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
// Quantum Smoke sigil — pointy-top hexagon + inner diamond + crosshairs + smoke wisps
static void drawSplashLogo(TFT_eSPI &t, int cx, int cy) {
    const uint16_t C = g_themeColor;

    // Pointy-top hexagon, r=36 (pre-computed vertices, no float)
    // Vertices: top, upper-right, lower-right, bottom, lower-left, upper-left
    static const int8_t HX[] = {  0, 31, 31,  0, -31, -31 };
    static const int8_t HY[] = { -36, -18, 18, 36,  18, -18 };
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        t.drawLine(cx + HX[i], cy + HY[i], cx + HX[j], cy + HY[j], C);
        t.drawLine(cx + HX[i] + (HX[i] > 0 ? -1 : HX[i] < 0 ? 1 : 0),
                   cy + HY[i] + (HY[i] > 0 ? -1 : HY[i] < 0 ? 1 : 0),
                   cx + HX[j] + (HX[j] > 0 ? -1 : HX[j] < 0 ? 1 : 0),
                   cy + HY[j] + (HY[j] > 0 ? -1 : HY[j] < 0 ? 1 : 0), C);  // double stroke
    }

    // Inner diamond (rotated square, r=18)
    t.drawLine(cx,      cy - 18, cx + 18, cy,      C);
    t.drawLine(cx + 18, cy,      cx,      cy + 18, C);
    t.drawLine(cx,      cy + 18, cx - 18, cy,      C);
    t.drawLine(cx - 18, cy,      cx,      cy - 18, C);

    // Cardinal crosshair ticks (extending outward from hex)
    t.drawFastVLine(cx, cy - 46, 8, C);   // top
    t.drawFastVLine(cx, cy + 38, 8, C);   // bottom
    t.drawFastHLine(cx - 40, cy, 8, C);   // left
    t.drawFastHLine(cx + 32, cy, 8, C);   // right

    // Centre pip
    t.fillCircle(cx, cy, 3, C);
    t.drawCircle(cx, cy, 6, C);

    // Smoke wisps — three tendrils rising from top vertex
    int wx = cx, wy = cy - 36;
    // Left wisp
    t.drawLine(wx - 2, wy,      wx - 6, wy - 9,  C);
    t.drawLine(wx - 6, wy - 9,  wx - 2, wy - 17, C);
    t.drawLine(wx - 2, wy - 17, wx - 5, wy - 24, C);
    // Centre wisp
    t.drawLine(wx,     wy - 2,  wx + 1, wy - 11, C);
    t.drawLine(wx + 1, wy - 11, wx - 2, wy - 20, C);
    t.drawLine(wx - 2, wy - 20, wx + 1, wy - 27, C);
    // Right wisp
    t.drawLine(wx + 2, wy,      wx + 6, wy - 9,  C);
    t.drawLine(wx + 6, wy - 9,  wx + 2, wy - 17, C);
    t.drawLine(wx + 2, wy - 17, wx + 5, wy - 24, C);

    // xX marks flanking the hex (brand: xXMayDayXx)
    for (int side = -1; side <= 1; side += 2) {
        int xx = cx + side * 48;
        t.drawLine(xx - 4, cy - 4, xx + 4, cy + 4, C);
        t.drawLine(xx + 4, cy - 4, xx - 4, cy + 4, C);
    }
}

static void showSplash() {
    tft.fillScreen(COL_BG);

    // Corner border
    drawCornerBrackets(tft, 2, 2, SCREEN_W - 4, SCREEN_H - 4, g_themeColor, 16);

    // Logo — moved up to leave room for larger text below
    drawSplashLogo(tft, SCREEN_W / 2, 72);

    // Brand name
    tft.setTextFont(FONT_LARGE);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("MAYDAY", SCREEN_W / 2, 114, FONT_LARGE);

    // Device name
    tft.setTextFont(FONT_MED);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("Ai-Field-Terminal", SCREEN_W / 2, 148, FONT_MED);

    // Studio tag
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawCentreString("xXQuantum-SmokeXx", SCREEN_W / 2, 184, FONT_SMALL);

    delay(2200);
    tft.fillScreen(COL_BG);
}

// ── Boot WiFi + NTP ───────────────────────────────────────────────────────────
static void bootWifi() {
    String ssid, pass;
    if (wifiLoadFromSD(ssid, pass)) wifiSaveCreds(ssid, pass);

    tft.fillScreen(COL_BG);
    tft.setTextFont(FONT_SMALL);
    tft.setTextColor(g_themeColor, COL_BG);
    tft.drawString("Connecting to WiFi...", 4, 10);

    if (wifiConnect()) {
        homeSetWifiStatus(true);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.drawString("Connected: " + wifiIP(), 4, 26);
        // NTP sync — Eastern time with auto DST
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
        tzset();
        tft.setTextColor(g_themeColor, COL_BG);
        tft.drawString("Syncing time...", 4, 42);
        uint32_t t0 = millis();
        while (time(nullptr) < 1000000 && millis() - t0 < 4000) delay(100);
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
            char tbuf[24];
            snprintf(tbuf, sizeof(tbuf), "Time: %02d:%02d ET", ti.tm_hour, ti.tm_min);
            tft.setTextColor(g_themeColor, COL_BG);
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

// ── Boot sync ─────────────────────────────────────────────────────────────────
// Fetches module data before the home screen appears.  A single status line
// shows which service is being fetched.  The hardware watchdog is disabled
// during sync (Solar alone calls 7+ NOAA endpoints) and restored afterwards.

#include <esp_task_wdt.h>

static void bootSyncAll() {
    if (!WiFi.isConnected()) return;

    // Wait for valid NTP time — caches depend on accurate timestamps for TTL.
    // bootWifi() only waits 4 s; on slow networks NTP may need longer.
    uint32_t nt0 = millis();
    while (time(nullptr) < 1000000 && millis() - nt0 < 10000) delay(200);
    if (time(nullptr) < 1000000) return;  // NTP failed — skip sync, fetch on entry

    // Crypto, Weather, Solar, Quakes — the rest (News, SHTF, Traffic)
    // sync on-entry where failures are isolated to one screen.
    static const char *LABELS[] = {
        "Crypto", "Weather", "Solar", "Quakes"
    };
    const int COUNT = 4;

    // Disable task watchdog during sync — individual HTTP calls can run 10-90 s.
    esp_task_wdt_deinit();

    for (int i = 0; i < COUNT; i++) {
        // Single changing status line at screen bottom
        char buf[40];
        snprintf(buf, sizeof(buf), "Syncing %s...", LABELS[i]);
        tft.fillRect(0, SCREEN_H - 22, SCREEN_W, 22, COL_BG);
        tft.setTextFont(FONT_SMALL);
        tft.setTextColor(g_themeColor, COL_BG);
        tft.drawString(buf, 4, SCREEN_H - 18);

        switch (i) {
            case 0: btcWarmCache();      break;
            case 1: weatherWarmCache();   break;
            case 2: solarWarmCache();     break;
            case 3: worldWarmCache();     break;
        }
    }

    // Leave task watchdog disabled — background HTTP refreshes in each
    // moduleʼs init can run 10-90 s and would trigger spurious resets.
    // The user can press the RST button if the device ever truly hangs.

    // Clear the status line
    tft.fillRect(0, SCREEN_H - 20, SCREEN_W, 20, COL_BG);
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
    initTouch();

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(COL_BG);
    themeColorInit();
    initBrightness();

    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_UP),    isrTbUp,    FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_DOWN),  isrTbDown,  FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_LEFT),  isrTbLeft,  FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_RIGHT), isrTbRight, FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_TBOX_CLICK), isrTbClick, FALLING);

    showSplash();
    loadPortalUrlFromSD();
    loadDonkiKeyFromSD();
    loadWazeKeyFromSD();
    loadSdKey("/tomtom.txt",       "tomtom_key");
    loadSdKey("/healthmap.txt",   "hmap_key");
    loadSdKey("/eia.txt",         "eia_key");
    loadSdKey("/poweroutage.txt", "poutage_key");
    bootWifi();
    personaMgrInit();

    // Fetch all module data before showing home screen.
    // Watchdog is fed between modules so long HTTP calls never trigger reset.
    bootSyncAll();

    homeInit(tft);
    touchDrain();
}

// ── loop ──────────────────────────────────────────────────────────────────────
void loop() {
    s_webServer.handleClient();  // non-blocking; serves /ss screenshot on demand

    if (s_screen == SCR_HOME) {
        handleHomeTrackball();
        handleHomeTouch();
        if (s_screen != SCR_HOME) return;  // touch launched a tile

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
        handleScreenTrackball();
        if (!chatLoop(tft)) returnHome();

    } else if (s_screen == SCR_WEATHER) {
        handleScreenTrackball();
        if (!weatherLoop(tft)) returnHome();

    } else if (s_screen == SCR_SOLAR) {
        handleScreenTrackball();
        if (!solarLoop(tft)) returnHome();

    } else if (s_screen == SCR_BTC) {
        handleScreenTrackball();
        if (!btcLoop(tft)) returnHome();

    } else if (s_screen == SCR_SYSINFO) {
        handleScreenTrackball();
        if (!sysinfoLoop(tft)) returnHome();

    } else if (s_screen == SCR_ALERTS) {
        handleScreenTrackball();
        handleModuleTouch();
        if (!noaaLoop(tft)) returnHome();

    } else if (s_screen == SCR_WORLD) {
        handleScreenTrackball();
        if (!worldLoop(tft)) returnHome();

    } else if (s_screen == SCR_HAZARD) {
        handleScreenTrackball();
        if (!wazeHazardLoop(tft)) returnHome();

    } else if (s_screen == SCR_ORACLE) {
        handleScreenTrackball();
        handleModuleTouch();
        if (!sigilLoop(tft)) returnHome();

    } else if (s_screen == SCR_ROAD) {
        handleScreenTrackball();
        if (!newsLoop(tft)) returnHome();

    } else if (s_screen == SCR_SHTF) {
        handleScreenTrackball();
        if (!shtfLoop(tft)) returnHome();

    } else {
        handleScreenTrackball();
        if (readKeyboard() != 0) returnHome();
        delay(20);
    }
}
