#include "noaa.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include <Wire.h>
#include <SD.h>
#include <time.h>

#define KB_ADDR          0x55
#define BOARD_SDCARD_CS  39
#define LOG_PATH         "/logs/field.log"
#define TMP_PATH         "/logs/field.tmp"
#define LOG_VISIBLE      11
#define LOG_ROW_H       16
#define INPUT_MAX        96

struct LogLine {
    String text;
    int    fileIndex;
};

static TFT_eSPI *s_tft = nullptr;
static LogLine   s_lines[LOG_VISIBLE];
static int       s_lineCount = 0;
static int       s_totalLines = 0;
static int       s_scrollOff = 0;
static int       s_selected = 0;
static int       s_editIndex = -1;
static bool      s_sdOk = false;
static bool      s_savedOk = false;
static String    s_input = "";
static char      s_status[30] = "SD: --";
static char      s_clockStr[12] = "";

static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

static void updateClockStr() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        int h = ti.tm_hour;
        const char *ap = h >= 12 ? "PM" : "AM";
        if (h > 12) h -= 12;
        else if (h == 0) h = 12;
        snprintf(s_clockStr, sizeof(s_clockStr), "%d:%02d %s", h, ti.tm_min, ap);
    } else {
        strlcpy(s_clockStr, "--:--", sizeof(s_clockStr));
    }
}

static String timestamp() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
        return String(buf);
    }
    return String("-- --:--");
}

static String stripLogPrefix(const String &line) {
    if (line.length() > 14 && line[0] == '[') {
        int close = line.indexOf("] ");
        if (close >= 0 && close + 2 < (int)line.length()) return line.substring(close + 2);
    }
    return line;
}

static String fitText(const String &text, int maxPx) {
    if (!s_tft) return text;
    String out = text;
    while (out.length() > 3 && s_tft->textWidth(out) > maxPx) out.remove(out.length() - 1);
    if (out.length() < text.length() && out.length() > 2) {
        out.remove(out.length() - 2);
        out += "..";
    }
    return out;
}

static bool ensureLogDir() {
    if (!SD.exists("/logs")) return SD.mkdir("/logs");
    return true;
}

static bool countLogLines() {
    s_totalLines = 0;
    File f = SD.open(LOG_PATH, FILE_READ);
    if (!f) return true;
    while (f.available()) {
        f.readStringUntil('\n');
        s_totalLines++;
    }
    f.close();
    return true;
}

static bool beginLogSd() {
    if (!SD.begin(BOARD_SDCARD_CS)) {
        s_sdOk = false;
        strlcpy(s_status, "SD: missing", sizeof(s_status));
        return false;
    }
    if (!ensureLogDir()) {
        SD.end();
        s_sdOk = false;
        strlcpy(s_status, "SD: mkdir fail", sizeof(s_status));
        return false;
    }
    return true;
}

static void clampSelection() {
    if (s_lineCount <= 0) s_selected = 0;
    else if (s_selected >= s_lineCount) s_selected = s_lineCount - 1;
    else if (s_selected < 0) s_selected = 0;
}

static void loadVisibleLines() {
    s_lineCount = 0;
    for (int i = 0; i < LOG_VISIBLE; i++) {
        s_lines[i].text = "";
        s_lines[i].fileIndex = -1;
    }

    if (!beginLogSd()) return;

    s_sdOk = countLogLines();
    if (!s_sdOk) {
        strlcpy(s_status, "SD: read fail", sizeof(s_status));
        SD.end();
        return;
    }

    int maxOff = max(0, s_totalLines - LOG_VISIBLE);
    if (s_scrollOff > maxOff) s_scrollOff = maxOff;
    if (s_scrollOff < 0) s_scrollOff = 0;
    int firstWanted = max(0, s_totalLines - LOG_VISIBLE - s_scrollOff);
    int lastWanted = min(s_totalLines, firstWanted + LOG_VISIBLE);

    File f = SD.open(LOG_PATH, FILE_READ);
    if (f) {
        int idx = 0;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (idx >= firstWanted && idx < lastWanted && s_lineCount < LOG_VISIBLE) {
                s_lines[s_lineCount].text = line;
                s_lines[s_lineCount].fileIndex = idx;
                s_lineCount++;
            }
            idx++;
        }
        f.close();
    }

    clampSelection();
    snprintf(s_status, sizeof(s_status), "SD: ok  LOGS:%d", s_totalLines);
    SD.end();
}

static bool rewriteLog(int targetIndex, const String *replacement, bool dropTarget) {
    if (!beginLogSd()) return false;

    File in = SD.open(LOG_PATH, FILE_READ);
    File out = SD.open(TMP_PATH, FILE_WRITE);
    if (!out) {
        if (in) in.close();
        SD.end();
        strlcpy(s_status, "SD: write fail", sizeof(s_status));
        return false;
    }

    int idx = 0;
    if (in) {
        while (in.available()) {
            String line = in.readStringUntil('\n');
            line.trim();
            if (idx == targetIndex) {
                if (!dropTarget && replacement) out.println(*replacement);
            } else if (line.length() > 0) {
                out.println(line);
            }
            idx++;
        }
        in.close();
    }
    out.close();

    SD.remove(LOG_PATH);
    bool ok = SD.rename(TMP_PATH, LOG_PATH);
    SD.end();
    if (!ok) strlcpy(s_status, "SD: rename fail", sizeof(s_status));
    return ok;
}

static bool appendLogLine(const String &entry) {
    if (!beginLogSd()) return false;
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) {
        SD.end();
        strlcpy(s_status, "SD: write fail", sizeof(s_status));
        return false;
    }
    f.println(entry);
    f.close();
    SD.end();
    s_scrollOff = 0;
    return true;
}

static void drawInput() {
    int y = SCREEN_H - 22;
    s_tft->fillRect(0, y, SCREEN_W, 22, COL_INPUT_BG);
    s_tft->drawFastHLine(0, y - 1, SCREEN_W, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_INPUT_BG);
    String display = s_editIndex >= 0 ? "E> " + s_input + "_" : "> " + s_input + "_";
    while (display.length() > 3 && s_tft->textWidth(display) > SCREEN_W - 4) display.remove(2, 1);
    s_tft->drawString(display, 2, y + 6);
}

static void drawLogScreen() {
    updateClockStr();
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "< HOME | LOG", s_clockStr, g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(s_sdOk ? g_themeColor : COL_AMBER, COL_BG);
    s_tft->drawString(s_status, 4, TOPBAR_H + 3);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("Q=home", SCREEN_W - s_tft->textWidth("Q=home") - 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int y = CONTENT_Y + 2;
    int areaH = SCREEN_H - CONTENT_Y - 24;
    s_tft->fillRect(0, CONTENT_Y, SCREEN_W, areaH, COL_BG);

    if (!s_sdOk) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawCentreString("SD REQUIRED", SCREEN_W / 2, y + 30, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("Q=home  Insert card for notes", SCREEN_W / 2, y + 54, FONT_SMALL);
    } else if (s_lineCount == 0) {
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("FIELD LOG", SCREEN_W / 2, y + 28, FONT_MED);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString("Type note, Enter saves, Q=home", SCREEN_W / 2, y + 52, FONT_SMALL);
    } else {
        s_tft->setTextFont(FONT_SMALL);
        for (int i = 0; i < s_lineCount; i++) {
            int ly = y + i * LOG_ROW_H;
            bool selected = i == s_selected;
            if (selected) {
                s_tft->fillRect(0, ly - 1, SCREEN_W, LOG_ROW_H, COL_INPUT_BG);
                drawCornerBrackets(*s_tft, 1, ly - 1, SCREEN_W - 2, LOG_ROW_H, g_themeColor, 5);
            }
            s_tft->setTextColor(g_themeColor, selected ? COL_INPUT_BG : COL_BG);
            s_tft->drawString(fitText(s_lines[i].text, SCREEN_W - 12), 6, ly + 2);
            if (i < s_lineCount - 1) s_tft->drawFastHLine(0, ly + LOG_ROW_H - 1, SCREEN_W, g_themeColor);
        }
    }

    if (s_savedOk) {
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawString("saved", SCREEN_W - 42, 4);
    }

    drawInput();
}

void noaaInit(TFT_eSPI &tft) {
    s_tft = &tft;
    s_input = "";
    s_scrollOff = 0;
    s_selected = 0;
    s_editIndex = -1;
    s_savedOk = false;
    loadVisibleLines();
    drawLogScreen();
}

bool noaaLoop(TFT_eSPI &tft) {
    (void)tft;
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    s_savedOk = false;
    if (key == 17 || key == 27) return false;
    if ((key == 'q' || key == 'Q') && s_input.isEmpty()) return false;

    if ((key == 'w' || key == 'W' || key == 'i') && s_lineCount > 0 && s_input.isEmpty()) {
        if (s_selected > 0) s_selected--;
        else noaaTrackballUp();
        drawLogScreen();
    } else if ((key == 's' || key == 'S' || key == 'k') && s_lineCount > 0 && s_input.isEmpty()) {
        if (s_selected < s_lineCount - 1) s_selected++;
        else noaaTrackballDown();
        drawLogScreen();
    } else if ((key == 'e' || key == 'E') && s_lineCount > 0 && s_input.isEmpty()) {
        s_editIndex = s_lines[s_selected].fileIndex;
        s_input = stripLogPrefix(s_lines[s_selected].text);
        drawLogScreen();
    } else if ((key == 'd' || key == 'D') && s_lineCount > 0 && s_input.isEmpty()) {
        if (rewriteLog(s_lines[s_selected].fileIndex, nullptr, true)) s_savedOk = true;
        s_editIndex = -1;
        loadVisibleLines();
        drawLogScreen();
    } else if (key == '\r' || key == '\n') {
        String msg = s_input;
        msg.trim();
        s_input = "";
        if (msg.length() > 0) {
            if (msg.length() > INPUT_MAX) msg = msg.substring(0, INPUT_MAX);
            String entry = "[" + timestamp() + "] " + msg;
            if (s_editIndex >= 0) {
                s_savedOk = rewriteLog(s_editIndex, &entry, false);
                s_editIndex = -1;
            } else {
                s_savedOk = appendLogLine(entry);
            }
            loadVisibleLines();
        } else {
            s_editIndex = -1;
        }
        drawLogScreen();
    } else if (key == 8 || key == 127) {
        if (s_input.length() > 0) s_input.remove(s_input.length() - 1);
        else s_editIndex = -1;
        drawInput();
    } else if (isprint((unsigned char)key) && s_input.length() < INPUT_MAX) {
        s_input += key;
        drawInput();
    }

    delay(20);
    return true;
}

void noaaTrackballUp() {
    s_savedOk = false;
    int maxOff = max(0, s_totalLines - LOG_VISIBLE);
    if (s_scrollOff < maxOff) {
        s_scrollOff++;
        loadVisibleLines();
        drawLogScreen();
    }
}

void noaaTrackballDown() {
    s_savedOk = false;
    if (s_scrollOff > 0) {
        s_scrollOff--;
        loadVisibleLines();
        drawLogScreen();
    }
}