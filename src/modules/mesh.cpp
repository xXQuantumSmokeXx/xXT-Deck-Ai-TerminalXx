#include "mesh.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include "../ui/home.h"
#include "../config/nvs_config.h"
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <time.h>

// ── T-Deck SX1262 pins ────────────────────────────────────────────────────────
#define RADIO_NSS    9
#define RADIO_DIO1  45
#define RADIO_RST   17
#define RADIO_BUSY  14

#define KB_ADDR      0x55
#define MAX_NODES    8

// ── LoRa region presets (Meshtastic LongFast params) ──────────────────────────
struct LoraPreset {
    const char *name;
    float       freq;   // MHz
    float       bw;     // kHz
    uint8_t     sf;
    uint8_t     cr;
};

static const LoraPreset PRESETS[] = {
    { "US915",  906.875f, 250.0f, 11, 5 },
    { "EU868",  869.525f, 250.0f, 11, 5 },
    { "ANZ915", 916.875f, 250.0f, 11, 5 },
    { "433MHz", 433.175f, 125.0f, 11, 5 },
};
#define PRESET_COUNT 4

// ── Node tracking ─────────────────────────────────────────────────────────────
struct MeshNode {
    uint32_t fromId;
    uint32_t toId;
    uint32_t lastHeardMs;
    int16_t  rssi;
    float    snr;
    bool     isBroadcast;
};

// ── Module-level state ────────────────────────────────────────────────────────
// Module + radio constructed at file scope — constructors only store references,
// no SPI transactions happen until radio.begin() is called in meshInit().
static Module  s_mod(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY, SPI);
static SX1262  s_radio(&s_mod);

static MeshNode s_nodes[MAX_NODES];
static int      s_nodeCount  = 0;
static int      s_pktTotal   = 0;
static int      s_pktBad     = 0;
static int      s_preset     = 0;
static bool     s_radioReady = false;
static TFT_eSPI *s_tft       = nullptr;

static volatile bool s_rxFlag = false;

void IRAM_ATTR radioISR() { s_rxFlag = true; }

// ── Keyboard ──────────────────────────────────────────────────────────────────
static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ── Radio init at current preset ─────────────────────────────────────────────
static bool initRadio() {
    const LoraPreset &p = PRESETS[s_preset];
    // Sync word 0x12 = Meshtastic private channel (all Meshtastic 2.x devices)
    int state = s_radio.begin(p.freq, p.bw, p.sf, p.cr, 0x12, 10, 16);
    if (state != RADIOLIB_ERR_NONE) return false;
    s_radio.setDio2AsRfSwitch(true);
    s_radio.setCRC(true);
    s_radio.setDio1Action(radioISR);
    s_radio.startReceive();
    return true;
}

// ── Parse Meshtastic MeshPacket protobuf header ───────────────────────────────
// MeshPacket fields: fixed32 to=1 (tag 0x0D), fixed32 from=2 (tag 0x15)
// These are always at the front of the packet in field-number order.
static bool parseMeshtasticHeader(const uint8_t *buf, int len,
                                   uint32_t &from, uint32_t &to) {
    if (len < 10) return false;
    from = 0; to = 0;
    bool gotFrom = false, gotTo = false;
    int i = 0;
    while (i < len) {
        if (i + 5 > len) break;
        uint8_t tag = buf[i++];
        if (tag == 0x0D) {                          // field 1, wire-type 5 (fixed32)
            memcpy(&to,   &buf[i], 4); i += 4;
            gotTo = true;
        } else if (tag == 0x15) {                   // field 2, wire-type 5 (fixed32)
            memcpy(&from, &buf[i], 4); i += 4;
            gotFrom = true;
        } else if (tag == 0x18) {                   // field 3, varint (packet id)
            // skip varint then stop — we have what we need
            while (i < len && (buf[i] & 0x80)) i++;
            break;
        } else {
            break;  // unexpected tag
        }
        if (gotFrom && gotTo) break;
    }
    return gotFrom && gotTo;
}

// ── Update node list ──────────────────────────────────────────────────────────
static void updateNodes(uint32_t fromId, uint32_t toId, int16_t rssi, float snr) {
    for (int i = 0; i < s_nodeCount; i++) {
        if (s_nodes[i].fromId == fromId) {
            s_nodes[i].lastHeardMs = millis();
            s_nodes[i].rssi        = rssi;
            s_nodes[i].snr         = snr;
            s_nodes[i].toId        = toId;
            s_nodes[i].isBroadcast = (toId == 0xFFFFFFFF);
            return;
        }
    }
    if (s_nodeCount < MAX_NODES) {
        int idx               = s_nodeCount++;
        s_nodes[idx].fromId      = fromId;
        s_nodes[idx].toId        = toId;
        s_nodes[idx].lastHeardMs = millis();
        s_nodes[idx].rssi        = rssi;
        s_nodes[idx].snr         = snr;
        s_nodes[idx].isBroadcast = (toId == 0xFFFFFFFF);
    }
}

// ── Handle a received LoRa packet ─────────────────────────────────────────────
static void handleReceived() {
    int pktLen = s_radio.getPacketLength();
    if (pktLen <= 0 || pktLen > 256) {
        s_radio.startReceive();
        return;
    }
    uint8_t buf[256];
    if (s_radio.readData(buf, pktLen) != RADIOLIB_ERR_NONE) {
        s_radio.startReceive();
        return;
    }
    int16_t rssi = (int16_t)s_radio.getRSSI();
    float   snr  = s_radio.getSNR();
    s_radio.startReceive();

    s_pktTotal++;
    uint32_t fromId = 0, toId = 0;
    if (parseMeshtasticHeader(buf, pktLen, fromId, toId))
        updateNodes(fromId, toId, rssi, snr);
    else
        s_pktBad++;
}

// ── Age string ────────────────────────────────────────────────────────────────
static void fmtAge(uint32_t nowMs, uint32_t thenMs, char *out, int outLen) {
    uint32_t sec = (nowMs - thenMs) / 1000;
    if (sec < 60)        snprintf(out, outLen, "%us",  sec);
    else if (sec < 3600) snprintf(out, outLen, "%um",  sec / 60);
    else                 snprintf(out, outLen, "%uh",  sec / 3600);
}

// ── Draw full screen ──────────────────────────────────────────────────────────
static void drawMeshScreen() {
    s_tft->fillScreen(COL_BG);

    // Topbar
    char rtBuf[12] = "";
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(rtBuf, sizeof(rtBuf), "%d:%02d %s", h, ti.tm_min, ap);
    }
    drawTopbar(*s_tft, "< HOME | MESH", rtBuf, COL_PURPLE);

    // Status bar
    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    if (s_radioReady) {
        const LoraPreset &p = PRESETS[s_preset];
        char buf[44];
        snprintf(buf, sizeof(buf), "%s  %.3fMHz  SF%u  BW%.0f",
                 p.name, p.freq, p.sf, p.bw);
        s_tft->setTextColor(COL_PURPLE, COL_BG);
        s_tft->drawString(buf, 4, TOPBAR_H + 3);
    } else {
        s_tft->setTextColor(COL_RED, COL_BG);
        s_tft->drawString("RADIO ERROR", 4, TOPBAR_H + 3);
    }
    char rxBuf[14];
    snprintf(rxBuf, sizeof(rxBuf), "Rx:%d Bad:%d", s_pktTotal, s_pktBad);
    int rxW = s_tft->textWidth(rxBuf);
    s_tft->setTextColor(COL_GREY_MID, COL_BG);
    s_tft->drawString(rxBuf, SCREEN_W - rxW - 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, COL_GREY_DIM);

    int y = TOPBAR_H + STATUSBAR_H + 2;

    if (!s_radioReady) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_RED, COL_BG);
        s_tft->drawCentreString("RADIO INIT FAILED", SCREEN_W / 2, y + 28, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_GREY_DIM, COL_BG);
        s_tft->drawCentreString("Check SX1262 wiring", SCREEN_W / 2, y + 50, FONT_SMALL);
        s_tft->drawCentreString("Q=home  F=region", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
        return;
    }

    if (s_nodeCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_GREY_DIM, COL_BG);
        s_tft->drawCentreString("SCANNING...", SCREEN_W / 2, y + 28, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_GREY_DIM, COL_BG);
        s_tft->drawCentreString("Listening for Meshtastic nodes", SCREEN_W / 2, y + 50, FONT_SMALL);
    } else {
        // Column headers
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_GREY_DIM, COL_BG);
        s_tft->drawString("NODE",   4,   y);
        s_tft->drawString("RSSI",   112, y);
        s_tft->drawString("SNR",    154, y);
        s_tft->drawString("AGE",    192, y);
        s_tft->drawString("DEST",   232, y);
        y += 10;
        s_tft->drawFastHLine(0, y, SCREEN_W, COL_GREY_DIM);
        y += 3;

        uint32_t now = millis();
        for (int i = 0; i < s_nodeCount; i++) {
            const MeshNode &n = s_nodes[i];

            // Source node ID: !xxxxxxxx
            char idBuf[12];
            snprintf(idBuf, sizeof(idBuf), "!%08x", (unsigned)n.fromId);
            s_tft->setTextColor(COL_PURPLE, COL_BG);
            s_tft->drawString(idBuf, 4, y);

            // RSSI (colour-coded)
            char rssiBuf[8];
            snprintf(rssiBuf, sizeof(rssiBuf), "%d", (int)n.rssi);
            uint16_t rssiCol = n.rssi >= -70 ? COL_CYAN
                             : n.rssi >= -85 ? COL_AMBER
                             : COL_RED;
            s_tft->setTextColor(rssiCol, COL_BG);
            s_tft->drawString(rssiBuf, 112, y);

            // SNR
            char snrBuf[8];
            snprintf(snrBuf, sizeof(snrBuf), "%.1f", n.snr);
            s_tft->setTextColor(COL_WHITE, COL_BG);
            s_tft->drawString(snrBuf, 154, y);

            // Age
            char ageBuf[8];
            fmtAge(now, n.lastHeardMs, ageBuf, sizeof(ageBuf));
            s_tft->setTextColor(COL_GREY_MID, COL_BG);
            s_tft->drawString(ageBuf, 192, y);

            // Destination
            if (n.isBroadcast) {
                s_tft->setTextColor(COL_GREY_DIM, COL_BG);
                s_tft->drawString("BCAST", 232, y);
            } else {
                char dstBuf[12];
                snprintf(dstBuf, sizeof(dstBuf), "!%08x", (unsigned)n.toId);
                s_tft->setTextColor(COL_GREY_MID, COL_BG);
                s_tft->drawString(dstBuf, 232, y);
            }

            y += 13;
        }
    }

    // Hint bar
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_GREY_DIM, COL_BG);
    s_tft->drawCentreString("Q=home  R=reset  F=region  J=restart", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
}

// ── Confirm-and-restart screen ────────────────────────────────────────────────
static void doRestart() {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "< HOME | MESH", "", COL_PURPLE);
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(COL_AMBER, COL_BG);
    s_tft->drawCentreString("RESTART DEVICE?", SCREEN_W / 2, SCREEN_H / 2 - 22, FONT_MED);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(COL_GREY_MID, COL_BG);
    s_tft->drawCentreString("Use with M5Launcher or", SCREEN_W / 2, SCREEN_H / 2 + 4, FONT_SMALL);
    s_tft->drawCentreString("a dual-boot partition setup.", SCREEN_W / 2, SCREEN_H / 2 + 16, FONT_SMALL);
    s_tft->drawCentreString("Y=confirm  N=cancel", SCREEN_W / 2, SCREEN_H - 12, FONT_SMALL);
    while (true) {
        char k = readKeyboard();
        if (k == 'y' || k == 'Y') {
            s_tft->fillScreen(COL_BG);
            s_tft->setTextFont(FONT_MED);
            s_tft->setTextColor(COL_PURPLE, COL_BG);
            s_tft->drawCentreString("Restarting...", SCREEN_W / 2, SCREEN_H / 2 - 8, FONT_MED);
            delay(800);
            ESP.restart();
        }
        if (k == 'n' || k == 'N' || k == 27 || k == 17) return;
        delay(20);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void meshInit(TFT_eSPI &tft) {
    s_tft = &tft;

    s_preset = nvsGetInt("mesh_region", 0);
    if (s_preset < 0 || s_preset >= PRESET_COUNT) s_preset = 0;

    // Clear screen immediately so home menu doesn't show during radio init
    tft.fillScreen(COL_BG);
    drawTopbar(tft, "< HOME | MESH", "", COL_PURPLE);

    s_radioReady = initRadio();
    s_rxFlag     = false;

    if (s_radioReady) homeSetLoraStatus(true);

    drawMeshScreen();
}

bool meshLoop(TFT_eSPI &tft) {
    // Process any ISR-flagged packet
    if (s_rxFlag) {
        s_rxFlag = false;
        handleReceived();
        drawMeshScreen();
    }

    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'q' || key == 'Q' || key == 27 || key == 17 || key == 8) {
        s_radio.standby();
        return false;
    }

    if (key == 'r' || key == 'R') {
        s_nodeCount = 0;
        s_pktTotal  = 0;
        s_pktBad    = 0;
        if (s_radioReady) s_radio.startReceive();
        drawMeshScreen();
    }

    if (key == 'f' || key == 'F') {
        s_preset = (s_preset + 1) % PRESET_COUNT;
        nvsPutInt("mesh_region", s_preset);
        s_nodeCount = 0;
        s_pktTotal  = 0;
        s_pktBad    = 0;
        s_radioReady = initRadio();
        drawMeshScreen();
    }

    if (key == 'j' || key == 'J') {
        doRestart();
        drawMeshScreen();
    }

    delay(20);
    return true;
}
