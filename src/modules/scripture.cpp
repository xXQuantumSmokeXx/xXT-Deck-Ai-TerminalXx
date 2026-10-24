#include "scripture.h"
#include "../ui/theme.h"
#include "../ui/widgets.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>

#define BOARD_SDCARD_CS  39
#define KB_ADDR          0x55
#define SCRIP_DIR        "/scripture"
#define MYBOOKS_DIR      "/scripture/mybooks"
#define MYBOOKS_MAX      20
#define SCRIP_VISIBLE    11   // rows in book-select list (FONT_SMALL)
#define SCRIP_ROW_H      16   // row height in book-select list
#define SCRIP_WRAP_W     (SCREEN_W - 8)
#define PAGE_HIST_MAX    200
#define IDX_MAX          280  // Book of Mormon alone has 239 chapters
#define IDX_VISIBLE      10   // rows in chapter-index browser (FONT_MED)
#define IDX_ROW_H        18   // row height in chapter-index browser
#define READ_VISIBLE     10   // rows in reader (FONT_MED)
#define READ_ROW_H       18   // row height in reader (16px font + 2px gap)

// ─── Built-in scripture books ─────────────────────────────────────────────────

struct ScriptureBook {
    const char* title;
    const char* shortTitle;
    const char* url;
    const char* filename;
};

static const ScriptureBook BOOKS[] = {
    {"KJV Bible",            "BIBLE",  "https://www.gutenberg.org/cache/epub/10/pg10.txt",        "/scripture/bible.txt"},
    {"Quran (Rodwell)",      "QURAN",  "https://www.gutenberg.org/cache/epub/2800/pg2800.txt",    "/scripture/quran.txt"},
    {"Book of Mormon",       "MORMON", "https://www.gutenberg.org/cache/epub/17/pg17.txt",        "/scripture/mormon.txt"},
    {"Tao Te Ching",         "TAO",    "https://www.gutenberg.org/cache/epub/216/pg216.txt",      "/scripture/tao.txt"},
    {"Bhagavad Gita",        "GITA",   "https://www.gutenberg.org/cache/epub/2388/pg2388.txt",    "/scripture/gita.txt"},
    {"Egyptian Bk. of Dead", "EGYPT",  "https://www.gutenberg.org/cache/epub/69566/pg69566.txt",  "/scripture/egypt.txt"},
};
static const int BOOK_COUNT = 6;

// ─── My Books (SD card, auto-scanned) ────────────────────────────────────────

struct MyBook {
    char title[32];
    char shortTitle[8];
    char filename[52];
};

static MyBook s_myBooks[MYBOOKS_MAX];
static int    s_myBookCount = 0;

// ─── Display row layout ───────────────────────────────────────────────────────
// Layout:  [SCRIPTURE header] [6 scripture books] [MY BOOKS header] [N books | hint]

enum RowType { ROW_SCRIPTURE_HDR, ROW_MY_BOOKS_HDR, ROW_BOOK, ROW_HINT };

struct DisplayRow { RowType type; int bookIdx; };

static int totalBookCount()   { return BOOK_COUNT + s_myBookCount; }
static bool isCustomBook(int idx) { return idx >= BOOK_COUNT; }

static int totalDisplayRows() {
    return 1 + BOOK_COUNT + 1 + (s_myBookCount > 0 ? s_myBookCount : 1);
}

static DisplayRow getDisplayRow(int row) {
    DisplayRow dr;
    if (row == 0) {
        dr.type = ROW_SCRIPTURE_HDR; dr.bookIdx = -1;
    } else if (row <= BOOK_COUNT) {
        dr.type = ROW_BOOK; dr.bookIdx = row - 1;
    } else if (row == BOOK_COUNT + 1) {
        dr.type = ROW_MY_BOOKS_HDR; dr.bookIdx = -1;
    } else if (s_myBookCount == 0) {
        dr.type = ROW_HINT; dr.bookIdx = -1;
    } else {
        int myIdx = row - BOOK_COUNT - 2;
        dr.type    = (myIdx < s_myBookCount) ? ROW_BOOK : ROW_HINT;
        dr.bookIdx = (myIdx < s_myBookCount) ? BOOK_COUNT + myIdx : -1;
    }
    return dr;
}

static int bookToDisplayRow(int bookIdx) {
    return (bookIdx < BOOK_COUNT) ? bookIdx + 1 : BOOK_COUNT + 2 + (bookIdx - BOOK_COUNT);
}

// ─── State ────────────────────────────────────────────────────────────────────

enum ScripState { SS_BOOK_SELECT, SS_DOWNLOADING, SS_NO_WIFI, SS_READING, SS_INDEX };

struct IdxEntry {
    uint32_t offset;
    char     title[56];
};

static TFT_eSPI  *s_tft          = nullptr;
static ScripState s_state         = SS_BOOK_SELECT;
static int        s_bookSel       = 0;
static int        s_bookScrollOff = 0;
static bool       s_onSD[BOOK_COUNT];

static uint32_t   s_pageHist[PAGE_HIST_MAX];
static int        s_histDepth = 0;
static uint32_t   s_pageStart = 0;
static uint32_t   s_pageNext  = 0;
static uint32_t   s_fileSize  = 0;
static File       s_file;
static bool       s_fileOpen  = false;
static char       s_bookPath[52];
static char       s_bookShort[8];
static IdxEntry   s_idx[IDX_MAX];
static int        s_idxCount     = 0;
static int        s_idxSel       = 0;
static int        s_idxScrollOff = 0;
static bool       s_goHome       = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static char readKeyboard() {
    char key = 0;
    Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1);
    while (Wire.available()) key = Wire.read();
    return key;
}

// ─── Book accessors ───────────────────────────────────────────────────────────

static const char* getBookTitle(int idx) {
    return isCustomBook(idx) ? s_myBooks[idx - BOOK_COUNT].title     : BOOKS[idx].title;
}
static const char* getBookShort(int idx) {
    return isCustomBook(idx) ? s_myBooks[idx - BOOK_COUNT].shortTitle : BOOKS[idx].shortTitle;
}
static const char* getBookFile(int idx) {
    return isCustomBook(idx) ? s_myBooks[idx - BOOK_COUNT].filename   : BOOKS[idx].filename;
}
static const char* getBookUrl(int idx) {
    return isCustomBook(idx) ? nullptr : BOOKS[idx].url;
}
static bool isBookOnSD(int idx) {
    return isCustomBook(idx) ? true : s_onSD[idx];
}

// ─── Scroll / selection ───────────────────────────────────────────────────────

static void ensureBookVisible() {
    int selRow = bookToDisplayRow(s_bookSel);
    if (selRow < s_bookScrollOff) s_bookScrollOff = selRow;
    if (selRow >= s_bookScrollOff + SCRIP_VISIBLE)
        s_bookScrollOff = selRow - SCRIP_VISIBLE + 1;
}

static void bookSelUp() {
    if (s_bookSel > 0) { s_bookSel--; ensureBookVisible(); }
}

static void bookSelDown() {
    if (s_bookSel < totalBookCount() - 1) { s_bookSel++; ensureBookVisible(); }
}

// ─── Filename → title helpers ─────────────────────────────────────────────────

static void titleFromFilename(const char *filename, char *title, int len) {
    const char *dot = strrchr(filename, '.');
    int n = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (n >= len) n = len - 1;
    strncpy(title, filename, n);
    title[n] = '\0';
    for (int i = 0; i < n; i++)
        if (title[i] == '-' || title[i] == '_') title[i] = ' ';
    bool cap = true;
    for (int i = 0; i < n; i++) {
        if (title[i] == ' ') { cap = true; }
        else if (cap) { title[i] = toupper((unsigned char)title[i]); cap = false; }
    }
}

static void shortFromTitle(const char *title, char *shortTitle, int len) {
    int i = 0;
    while (title[i] && title[i] != ' ' && i < len - 1) {
        shortTitle[i] = toupper((unsigned char)title[i]);
        i++;
    }
    shortTitle[i] = '\0';
}

// ─── SD cache + My Books scan ─────────────────────────────────────────────────

static void checkSDCache() {
    memset(s_onSD, 0, sizeof(s_onSD));
    s_myBookCount = 0;

    if (!SD.begin(BOARD_SDCARD_CS)) return;

    for (int i = 0; i < BOOK_COUNT; i++)
        s_onSD[i] = SD.exists(BOOKS[i].filename);

    if (!SD.exists(MYBOOKS_DIR)) SD.mkdir(MYBOOKS_DIR);

    File dir = SD.open(MYBOOKS_DIR);
    if (dir && dir.isDirectory()) {
        File entry;
        while (s_myBookCount < MYBOOKS_MAX) {
            entry = dir.openNextFile();
            if (!entry) break;
            if (!entry.isDirectory()) {
                const char *name = entry.name();
                // Some SD libs return full path — keep only basename
                const char *base = strrchr(name, '/');
                base = base ? base + 1 : name;
                int blen = strlen(base);
                if (base[0] != '.' && blen >= 5 &&
                    strcasecmp(base + blen - 4, ".txt") == 0) {
                    MyBook &mb = s_myBooks[s_myBookCount];
                    titleFromFilename(base, mb.title, sizeof(mb.title));
                    shortFromTitle(mb.title, mb.shortTitle, sizeof(mb.shortTitle));
                    snprintf(mb.filename, sizeof(mb.filename), "%s/%s", MYBOOKS_DIR, base);
                    s_myBookCount++;
                }
            }
            entry.close();
        }
        dir.close();
    }

    SD.end();
}

// ─── File open / close ────────────────────────────────────────────────────────

static bool openFile(const char *path) {
    if (s_fileOpen) { s_file.close(); s_fileOpen = false; SD.end(); }
    s_fileSize = 0;
    if (!SD.begin(BOARD_SDCARD_CS)) return false;
    if (!SD.exists(path)) { SD.end(); return false; }
    { File tmp = SD.open(path, FILE_READ); if (tmp) { s_fileSize = tmp.size(); tmp.close(); } }
    s_file = SD.open(path, FILE_READ);
    if (!s_file) { SD.end(); return false; }
    s_fileOpen = true;
    return true;
}

static void closeFile() {
    if (s_fileOpen) { s_file.close(); s_fileOpen = false; }
    SD.end();
}

// ─── Chapter index ────────────────────────────────────────────────────────────

#define IDX_VERSION  0x07   // bump to auto-invalidate cached .idx files

static void mkIdxPath(const char *bookPath, char *out, int outLen) {
    strlcpy(out, bookPath, outLen);
    int len = strlen(out);
    if (len > 4 && strcasecmp(out + len - 4, ".txt") == 0)
        strcpy(out + len - 4, ".idx");
    else
        strlcat(out, ".idx", outLen);
}

// Case/punctuation-insensitive title comparison for dedup.
// "The First Book of Moses: Called Genesis" == "THE FIRST BOOK OF MOSES CALLED GENESIS"
// Digits are significant: without them every "1 Nephi Chapter 1..239" collapses
// into one entry, which is exactly what happened before 2026-10-23.
static bool titleNormEq(const char *a, const char *b) {
    while (*a || *b) {
        while (*a && !isalnum((uint8_t)*a)) a++;
        while (*b && !isalnum((uint8_t)*b)) b++;
        if (!*a && !*b) return true;
        if (!*a || !*b) return false;
        if (tolower((uint8_t)*a) != tolower((uint8_t)*b)) return false;
        a++; b++;
    }
    return true;
}

// ── Heading shape tests (2026-10-23) ─────────────────────────────────────────
// The blank-line sandwich alone missed two common layouts: headings that start
// with a number ("1 Nephi Chapter 1") and headings that sit directly above
// prose with no trailing blank ("SURA LXXIV"). A keyword or an inline
// "Chapter N" is structure the file is stating outright, so those are indexed
// without waiting for a blank line. Front matter is skipped by name.
static bool excludedHeading(const String &lower) {
    static const char *BAD[] = {
        "endeth", "end of", "here end", "contents", "table of",
        "title:", "language:", "credits:", "translator:", "author of",
        "produced by", "html version", "start of the project",
        "end of the project", "transcriber",
    };
    for (const char *b : BAD) if (lower.indexOf(b) >= 0) return true;
    return false;
}

static bool keywordHeading(const String &lower) {
    static const char *KW[] = {
        "chapter", "book", "part", "sura", "surah", "canto",
        "act", "scene", "psalm", "section", "hymn", "appendix",
    };
    for (const char *k : KW) {
        size_t l = strlen(k);
        if (!lower.startsWith(k)) continue;
        if (lower.length() == l) return true;
        char c = lower[l];
        if (c == ' ' || c == '.' || c == ':' || isdigit((uint8_t)c)) return true;
    }
    return false;
}

// "1 Nephi Chapter 12", "Book of Alma Chapter 3" — the chapter number can sit
// anywhere on the line, because some editions prefix the book name.
static bool inlineChapterNumber(const String &lower) {
    int p = lower.indexOf("chapter ");
    if (p < 0) return false;
    size_t i = (size_t)p + 8;
    while (i < lower.length() && lower[i] == ' ') i++;
    if (i >= lower.length()) return false;
    char c = lower[i];
    return isdigit((uint8_t)c) || c == 'i' || c == 'v' || c == 'x' ||
           c == 'l' || c == 'c' || c == 'd' || c == 'm';
}

// A bare "12." or "IX." line: chapter markers in books that put the number on
// its own line (Legge's Tao uses "12." followed by the chapter's first verse).
static bool numericMarker(const String &tr) {
    int i = 0;
    while (i < (int)tr.length() && isdigit((uint8_t)tr[i])) i++;
    if (i == 0 || i > 3) return false;
    while (i < (int)tr.length() && tr[i] == ' ') i++;
    return (i == (int)tr.length()) || (i == (int)tr.length() - 1 && tr[i] == '.');
}

static void addIndexEntry(uint32_t offset, const char *title) {
    for (int d = 0; d < s_idxCount; d++) {
        if (titleNormEq(s_idx[d].title, title)) {
            // Same title seen before (TOC copy) — update to the later offset
            s_idx[d].offset = offset;
            strlcpy(s_idx[d].title, title, sizeof(IdxEntry::title));
            return;
        }
    }
    if (s_idxCount < IDX_MAX) {
        s_idx[s_idxCount].offset = offset;
        strlcpy(s_idx[s_idxCount].title, title, sizeof(IdxEntry::title));
        s_idxCount++;
    }
}

// Scan bookPath and record lines that are structural section headers.
//
// Sandwich heuristic: a line is a header only if it has a blank line
// BOTH before AND after it. This rejects:
//   - TOC book listings (next line is another TOC entry, no blank after)
//   - Psalm superscriptions (followed immediately by verse text)
//   - Mid-sentence verse continuation lines
//
// Normalized dedup: if a TOC entry and the actual book header match
// (ignoring case/punctuation), the later (higher-offset) occurrence
// replaces the earlier one so jumps land in the actual content.
// Entries are sorted by offset before saving.
static void buildBookIndex(const char *bookPath, const char *idxPath) {
    s_idxCount = 0;

    s_tft->fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("Building chapter index...", SCREEN_W / 2, CONTENT_Y + 40, FONT_SMALL);
    s_tft->drawCentreString("(one-time scan)", SCREEN_W / 2, CONTENT_Y + 56, FONT_SMALL);

    if (!SD.begin(BOARD_SDCARD_CS)) return;
    File f = SD.open(bookPath, FILE_READ);
    if (!f) { SD.end(); return; }

    int blanksBefore = 3;  // pretend we started after blank lines

    bool     pendingOk     = false;
    uint32_t pendingOffset = 0;
    char     pendingTitle[sizeof(IdxEntry::title)] = "";

    while (f.available()) {
        uint32_t lineStart = f.position();
        String   line      = f.readStringUntil('\n');
        String   tr        = line;
        tr.trim();

        if (tr.length() == 0) {
            blanksBefore++;
            if (pendingOk) {
                // Blank after candidate → sandwich confirmed
                addIndexEntry(pendingOffset, pendingTitle);
                pendingOk = false;
            }
            continue;
        }

        // Non-blank line: reject any unconfirmed pending candidate
        pendingOk = false;

        String lower = tr;
        lower.toLowerCase();

        if (blanksBefore >= 1 &&
            tr.length() >= 3 && tr.length() <= 70 &&
            !excludedHeading(lower)) {

            // Explicit structure — "CHAPTER IV", "1 Nephi Chapter 1",
            // "SURA LXXIV". Indexed immediately: the body heading can sit
            // straight above prose, and a TOC copy is replaced later because
            // dedup keeps the last occurrence of a title.
            if (keywordHeading(lower) || inlineChapterNumber(lower)) {
                addIndexEntry(lineStart, tr.c_str());
            } else if (tr.indexOf("....") < 0) {
                // Otherwise wait for a blank line after it (sandwich), which
                // rejects TOC runs and verse paragraphs.
                bool numeric    = numericMarker(tr);
                bool alphaStart = isalpha((uint8_t)tr[0]) &&
                                  !isdigit((uint8_t)tr[tr.length() - 1]);
                if (numeric || alphaStart) {
                    pendingOk     = true;
                    pendingOffset = lineStart;
                    strlcpy(pendingTitle, tr.c_str(), sizeof(pendingTitle));
                }
            }
        }

        blanksBefore = 0;
    }
    f.close();

    // Sort entries by byte offset so the browser lists them in file order
    for (int i = 0; i < s_idxCount - 1; i++)
        for (int j = i + 1; j < s_idxCount; j++)
            if (s_idx[j].offset < s_idx[i].offset) {
                IdxEntry tmp = s_idx[i]; s_idx[i] = s_idx[j]; s_idx[j] = tmp;
            }

    // Merge consecutive entries within 150 bytes — handles Gutenberg dual-header pattern
    // (long title "The First Book of Moses: Called Genesis" immediately followed by
    //  short running title "Genesis"). Keep the longer title at the earlier offset.
    for (int i = 0; i < s_idxCount - 1; ) {
        if (s_idx[i + 1].offset - s_idx[i].offset < 150) {
            if (strlen(s_idx[i + 1].title) > strlen(s_idx[i].title))
                strlcpy(s_idx[i].title, s_idx[i + 1].title, sizeof(IdxEntry::title));
            for (int k = i + 1; k < s_idxCount - 1; k++) s_idx[k] = s_idx[k + 1];
            s_idxCount--;
        } else {
            i++;
        }
    }

    File idx = SD.open(idxPath, FILE_WRITE);
    if (idx) {
        uint8_t  ver = IDX_VERSION;
        uint32_t cnt = (uint32_t)s_idxCount;
        idx.write(&ver, 1);
        idx.write((uint8_t *)&cnt, 4);
        for (int i = 0; i < s_idxCount; i++)
            idx.write((uint8_t *)&s_idx[i], sizeof(IdxEntry));
        idx.close();
    }
    SD.end();
}

static void loadBookIndex(const char *idxPath) {
    s_idxCount = 0;
    if (!SD.begin(BOARD_SDCARD_CS)) return;
    File idx = SD.open(idxPath, FILE_READ);
    if (!idx) { SD.end(); return; }
    uint32_t fileSize = idx.size();
    // Read and validate version byte
    uint8_t ver = 0;
    idx.read(&ver, 1);
    if (ver != IDX_VERSION) { idx.close(); SD.end(); return; }
    uint32_t cnt = 0;
    idx.read((uint8_t *)&cnt, 4);
    // Validate stride — rejects files built with a different IdxEntry size
    if (cnt == 0 || fileSize < 5 + cnt * (uint32_t)sizeof(IdxEntry) ||
        (fileSize - 5) % sizeof(IdxEntry) != 0) {
        idx.close();
        SD.end();
        return;
    }
    if (cnt > IDX_MAX) cnt = IDX_MAX;
    for (uint32_t i = 0; i < cnt; i++)
        idx.read((uint8_t *)&s_idx[i], sizeof(IdxEntry));
    s_idxCount = (int)cnt;
    idx.close();
    SD.end();
}

// ─── Word-wrap helpers ────────────────────────────────────────────────────────

static String fitToWidth(const String &text) {
    if (!s_tft || s_tft->textWidth(text) <= SCRIP_WRAP_W) return text;
    String chunk = text;
    while (chunk.length() > 1) {
        int sp = chunk.lastIndexOf(' ');
        if (sp > 0) chunk = chunk.substring(0, sp);
        else         chunk.remove(chunk.length() - 1);
        if (s_tft->textWidth(chunk) <= SCRIP_WRAP_W) break;
    }
    return chunk;
}

static int countWrappedRows(const String &text) {
    if (text.length() == 0) return 1;
    String rem = text;
    int rows = 0;
    while (rem.length() > 0) {
        String chunk = fitToWidth(rem);
        rows++;
        if (chunk.length() >= rem.length()) break;
        rem = rem.substring(chunk.length());
        rem.trim();
    }
    return rows;
}

// ─── Page renderer ────────────────────────────────────────────────────────────

// Returns the byte offset that starts the next page, or 0 at EOF.
// Paragraph-reflow mode: consecutive non-blank lines are joined with a space
// so that soft-wrapped source lines (e.g. Gutenberg 70-char wrapping) render
// as flowing prose rather than broken sentences.
static uint32_t renderPage(uint32_t startOffset, bool draw) {
    if (!s_fileOpen) return 0;
    s_file.seek(startOffset);

    // Set font for both draw and measure paths so textWidth() is consistent
    s_tft->setTextFont(FONT_MED);
    if (draw) s_tft->fillRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, COL_BG);

    int dispRows = 0;

    while (s_file.available() && dispRows < READ_VISIBLE) {
        uint32_t paraStart = s_file.position();
        String   para      = "";
        bool     hadBlank  = false;

        // Accumulate non-blank lines into one paragraph
        while (s_file.available()) {
            String raw = s_file.readStringUntil('\n');
            raw.trim();
            if (raw.length() == 0) { hadBlank = true; break; }
            if (para.length() > 0) para += ' ';
            para += raw;
        }

        if (para.length() == 0) {
            // Blank spacer (empty paragraph or consecutive blanks)
            dispRows++;
            continue;
        }

        int paraRows = countWrappedRows(para);
        int need     = paraRows + (hadBlank ? 1 : 0);  // +1 for trailing blank spacer

        if (dispRows + need > READ_VISIBLE) {
            s_file.seek(paraStart);
            break;
        }

        if (draw) {
            s_tft->setTextColor(g_themeColor, COL_BG);
            String rem = para;
            while (rem.length() > 0 && dispRows < READ_VISIBLE) {
                String chunk = fitToWidth(rem);
                s_tft->drawString(chunk, 4, CONTENT_Y + dispRows * READ_ROW_H + 2);
                dispRows++;
                if (chunk.length() >= rem.length()) break;
                rem = rem.substring(chunk.length());
                rem.trim();
            }
        } else {
            dispRows += paraRows;
        }

        if (hadBlank && dispRows < READ_VISIBLE) dispRows++;  // blank spacer after para
    }

    return s_file.available() ? (uint32_t)s_file.position() : 0;
}

// ─── Draw functions ───────────────────────────────────────────────────────────

static void drawSectionHeader(const char *label, int y) {
    int lineY = y + SCRIP_ROW_H / 2;
    s_tft->drawFastHLine(0, lineY, SCREEN_W, g_themeColor);
    s_tft->setTextFont(FONT_SMALL);
    int tw = s_tft->textWidth(label);
    int lx = (SCREEN_W - tw) / 2;
    s_tft->fillRect(lx - 4, y + 2, tw + 8, SCRIP_ROW_H - 4, COL_BG);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString(label, lx, y + 3);
}

static void drawBookSelect() {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "< CODEX | LIBRARY", "", g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("* = saved   Enter/tap=open   Q=back", 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int totalRows = totalDisplayRows();
    int y = CONTENT_Y + 2;

    for (int vis = 0; vis < SCRIP_VISIBLE; vis++) {
        int dRow = s_bookScrollOff + vis;
        if (dRow >= totalRows) break;

        int        ly = y + vis * SCRIP_ROW_H;
        DisplayRow dr = getDisplayRow(dRow);

        switch (dr.type) {
            case ROW_SCRIPTURE_HDR:
                drawSectionHeader("SCRIPTURE", ly);
                break;

            case ROW_MY_BOOKS_HDR:
                drawSectionHeader("MY BOOKS", ly);
                break;

            case ROW_HINT:
                s_tft->setTextFont(FONT_SMALL);
                s_tft->setTextColor(g_themeColor, COL_BG);
                s_tft->drawString("  add .txt files to /scripture/mybooks/", 4, ly + 2);
                break;

            case ROW_BOOK: {
                bool sel = (dr.bookIdx == s_bookSel);
                if (sel)
                    drawCornerBrackets(*s_tft, 1, ly - 1, SCREEN_W - 2, SCRIP_ROW_H, g_themeColor, 5);
                String label = String(isBookOnSD(dr.bookIdx) ? " *  " : "    ")
                             + getBookTitle(dr.bookIdx);
                s_tft->setTextFont(FONT_SMALL);
                s_tft->setTextColor(g_themeColor, COL_BG);
                s_tft->drawString(label, 4, ly + 2);
                // Separator only between consecutive book rows
                int nextDRow = dRow + 1;
                if (nextDRow < totalRows && getDisplayRow(nextDRow).type == ROW_BOOK)
                    s_tft->drawFastHLine(0, ly + SCRIP_ROW_H - 1, SCREEN_W, g_themeColor);
                break;
            }
        }
    }
}

static void drawDownloadScreen(const char *title, int pct) {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "DOWNLOADING", "", g_themeColor);

    int y = CONTENT_Y + 24;
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString(title, SCREEN_W / 2, y, FONT_MED);

    y += 38;
    int barW = SCREEN_W - 24, barX = 12, barH = 14;
    s_tft->drawRect(barX, y, barW, barH, g_themeColor);
    if (pct >= 0) {
        int fillW = max(0, (int)((float)pct / 100.0f * (float)(barW - 2)));
        if (fillW > 0) s_tft->fillRect(barX + 1, y + 1, fillW, barH - 2, g_themeColor);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(g_themeColor, COL_BG);
        s_tft->drawCentreString(buf, SCREEN_W / 2, y + barH + 6, FONT_SMALL);
    }
}

static void drawNoWifi() {
    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "LIBRARY", "", g_themeColor);

    int y = CONTENT_Y + 30;
    s_tft->setTextFont(FONT_MED);
    s_tft->setTextColor(COL_AMBER, COL_BG);
    s_tft->drawCentreString("WIFI REQUIRED", SCREEN_W / 2, y, FONT_MED);
    y += 30;
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("First download needs WiFi.", SCREEN_W / 2, y, FONT_SMALL);
    y += 16;
    s_tft->drawCentreString("Once saved, works offline.", SCREEN_W / 2, y, FONT_SMALL);
    y += 24;
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawCentreString("B or Q = back", SCREEN_W / 2, y, FONT_SMALL);
}

static void drawReadScreen() {
    char topLeft[28];
    snprintf(topLeft, sizeof(topLeft), "< CODEX | %s", s_bookShort);

    char pctBuf[8] = "";
    if (s_fileSize > 0)
        snprintf(pctBuf, sizeof(pctBuf), "%d%%",
                 (int)((float)s_pageStart / (float)s_fileSize * 100.0f));

    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, topLeft, pctBuf, g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    s_tft->drawString("J=chapters  Spc=pg  B=books  Q=back", 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    s_pageNext = renderPage(s_pageStart, true);
}

// ─── Download (scripture books only) ─────────────────────────────────────────

static bool downloadBook(int idx) {
    drawDownloadScreen(BOOKS[idx].title, 0);

    if (!SD.begin(BOARD_SDCARD_CS)) {
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawCentreString("SD card missing", SCREEN_W / 2, CONTENT_Y + 60, FONT_SMALL);
        delay(2000);
        return false;
    }
    if (!SD.exists(SCRIP_DIR)) SD.mkdir(SCRIP_DIR);
    if (SD.exists(BOOKS[idx].filename)) SD.remove(BOOKS[idx].filename);

    File outFile = SD.open(BOOKS[idx].filename, FILE_WRITE);
    if (!outFile) {
        SD.end();
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawCentreString("SD write error", SCREEN_W / 2, CONTENT_Y + 60, FONT_SMALL);
        delay(2000);
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();  // TODO: pin ISRG Root X1 for production
    client.setTimeout(30);
    HTTPClient https;
    https.begin(client, BOOKS[idx].url);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    https.setTimeout(30000);

    int code = https.GET();
    if (code != 200) {
        outFile.close();
        SD.remove(BOOKS[idx].filename);
        SD.end();
        https.end();
        char msg[32];
        snprintf(msg, sizeof(msg), "HTTP error %d", code);
        s_tft->setTextFont(FONT_SMALL);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawCentreString(msg, SCREEN_W / 2, CONTENT_Y + 60, FONT_SMALL);
        delay(2000);
        return false;
    }

    int         totalLen = https.getSize();
    WiFiClient *stream   = https.getStreamPtr();

    // Skip Gutenberg preamble
    uint32_t headerBytes = 0;
    while ((stream->connected() || stream->available()) && headerBytes < 150000) {
        if (!stream->available()) { delay(1); continue; }
        String line = stream->readStringUntil('\n');
        headerBytes += line.length() + 1;
        if (line.indexOf("*** START OF THE PROJECT GUTENBERG") >= 0) break;
    }

    // Stream content to SD
    uint8_t  buf[2048];
    int32_t  written    = 0;
    uint32_t lastUpdate = 0;

    while (stream->connected() || stream->available()) {
        if (!stream->available()) { delay(1); continue; }
        int len = stream->readBytes(buf, sizeof(buf));
        if (len > 0) { outFile.write(buf, len); written += len; }
        if (millis() - lastUpdate > 400) {
            lastUpdate = millis();
            int pct = (totalLen > 0)
                ? (int)((float)(headerBytes + written) / (float)totalLen * 100.0f) : -1;
            drawDownloadScreen(BOOKS[idx].title, pct);
        }
    }

    outFile.close();
    https.end();
    SD.end();
    return written > 0;
}

// ─── State entry ─────────────────────────────────────────────────────────────

static void enterReading(int bookIdx) {
    strlcpy(s_bookPath,  getBookFile(bookIdx),  sizeof(s_bookPath));
    strlcpy(s_bookShort, getBookShort(bookIdx), sizeof(s_bookShort));
    s_histDepth = 0;
    s_pageStart = 0;
    s_pageNext  = 0;

    s_tft->fillScreen(COL_BG);
    drawTopbar(*s_tft, "OPENING...", "", g_themeColor);

    // Build chapter index on first open, load on subsequent opens
    char idxBuf[56];
    mkIdxPath(s_bookPath, idxBuf, sizeof(idxBuf));
    bool needBuild = true;
    if (SD.begin(BOARD_SDCARD_CS)) {
        needBuild = !SD.exists(idxBuf);
        SD.end();
    }
    if (needBuild) {
        buildBookIndex(s_bookPath, idxBuf);
    } else {
        loadBookIndex(idxBuf);
        if (s_idxCount == 0) buildBookIndex(s_bookPath, idxBuf);  // stale empty index — rebuild
    }

    if (!openFile(s_bookPath)) {
        s_tft->fillScreen(COL_BG);
        s_tft->setTextFont(FONT_MED);
        s_tft->setTextColor(COL_AMBER, COL_BG);
        s_tft->drawCentreString("FILE OPEN ERROR", SCREEN_W / 2, SCREEN_H / 2, FONT_MED);
        delay(2000);
        s_state = SS_BOOK_SELECT;
        checkSDCache();
        drawBookSelect();
        return;
    }

    // If there's an index, jump straight to the first indexed entry (skip the TOC)
    if (s_idxCount > 0) {
        s_pageStart = s_idx[0].offset;
        s_file.seek(s_pageStart);
    }

    s_state = SS_READING;
    drawReadScreen();
}

static void drawIndexBrowser();  // forward declaration

// ─── Per-state loop handlers ──────────────────────────────────────────────────

static bool loopBookSelect() {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 8 || key == 127 || key == 17 || key == 27) return false;  // Delete/ESC = back to mode selector
    if (key == 'q' || key == 'Q') { s_goHome = true; return false; }  // Q = home

    bool redraw = false;
    if (key == 'w' || key == 'W' || key == 'i') {
        bookSelUp(); redraw = true;
    } else if (key == 's' || key == 'S' || key == 'k') {
        bookSelDown(); redraw = true;
    } else if (key == '\r' || key == '\n') {
        if (isBookOnSD(s_bookSel)) {
            enterReading(s_bookSel);
        } else if (WiFi.status() != WL_CONNECTED) {
            s_state = SS_NO_WIFI;
            drawNoWifi();
        } else {
            s_state = SS_DOWNLOADING;
            bool ok = downloadBook(s_bookSel);
            checkSDCache();
            if (ok) {
                enterReading(s_bookSel);
            } else {
                s_state = SS_BOOK_SELECT;
                drawBookSelect();
            }
        }
        delay(20);
        return true;
    }

    if (redraw) drawBookSelect();
    delay(20);
    return true;
}

static bool loopReading() {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'b' || key == 'B' || key == 8 || key == 127 || key == 17 || key == 27) {
        s_tft->fillScreen(COL_BG);  // clear immediately — closeFile/SD.end() adds visible delay
        closeFile();
        s_state = SS_BOOK_SELECT;
        drawBookSelect();
        return true;
    }
    if (key == 'q' || key == 'Q') { closeFile(); s_goHome = true; return false; }
    if ((key == 'j' || key == 'J') && s_idxCount > 0) {
        s_idxSel = 0; s_idxScrollOff = 0;
        s_state = SS_INDEX;
        drawIndexBrowser();
        return true;
    }
    if (key == ' ' && s_pageNext > 0 && s_histDepth < PAGE_HIST_MAX) {
        s_pageHist[s_histDepth++] = s_pageStart;
        s_pageStart = s_pageNext;
        drawReadScreen();
    }

    delay(20);
    return true;
}

static bool loopNoWifi() {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }
    if (key == 17 || key == 27 || key == 'b' || key == 'B' || key == 'q' || key == 'Q') {
        s_state = SS_BOOK_SELECT;
        drawBookSelect();
    }
    delay(20);
    return true;
}

// ─── Index browser ────────────────────────────────────────────────────────────

static void ensureIdxVisible() {
    if (s_idxSel < s_idxScrollOff) s_idxScrollOff = s_idxSel;
    if (s_idxSel >= s_idxScrollOff + IDX_VISIBLE)
        s_idxScrollOff = s_idxSel - IDX_VISIBLE + 1;
}

static void drawIndexBrowser() {
    s_tft->fillScreen(COL_BG);
    char topLeft[28];
    snprintf(topLeft, sizeof(topLeft), "< %s | CHAPTERS", s_bookShort);
    drawTopbar(*s_tft, topLeft, "", g_themeColor);

    s_tft->fillRect(0, TOPBAR_H, SCREEN_W, STATUSBAR_H, COL_BG);
    s_tft->setTextFont(FONT_SMALL);
    s_tft->setTextColor(g_themeColor, COL_BG);
    char hint[40];
    snprintf(hint, sizeof(hint), "%d chapters   Enter/tap=jump   B=back", s_idxCount);
    s_tft->drawString(hint, 4, TOPBAR_H + 3);
    s_tft->drawFastHLine(0, TOPBAR_H + STATUSBAR_H - 1, SCREEN_W, g_themeColor);

    int y = CONTENT_Y + 2;
    s_tft->setTextFont(FONT_MED);
    for (int vis = 0; vis < IDX_VISIBLE; vis++) {
        int i = s_idxScrollOff + vis;
        if (i >= s_idxCount) break;
        int  ly  = y + vis * IDX_ROW_H;
        bool sel = (i == s_idxSel);
        if (sel)
            drawCornerBrackets(*s_tft, 1, ly - 1, SCREEN_W - 2, IDX_ROW_H, g_themeColor, 5);
        s_tft->setTextColor(g_themeColor, COL_BG);
        String title = s_idx[i].title;
        while (title.length() > 1 && s_tft->textWidth(title) > SCREEN_W - 12)
            title.remove(title.length() - 1);
        s_tft->drawString(title, 6, ly + 1);
        if (vis < IDX_VISIBLE - 1 && i + 1 < s_idxCount)
            s_tft->drawFastHLine(0, ly + IDX_ROW_H - 1, SCREEN_W, g_themeColor);
    }
}

static void jumpToChapter(int i) {
    if (i < 0 || i >= s_idxCount) return;
    if (s_histDepth < PAGE_HIST_MAX) s_pageHist[s_histDepth++] = s_pageStart;
    s_pageStart = s_idx[i].offset;
    s_state = SS_READING;
    drawReadScreen();
}

static bool loopIndex() {
    char key = readKeyboard();
    if (key == 0) { delay(20); return true; }

    if (key == 'b' || key == 'B' || key == 8 || key == 127 || key == 17 || key == 27) {
        s_state = SS_READING;
        drawReadScreen();
        return true;
    }
    if (key == 'q' || key == 'Q') { closeFile(); s_goHome = true; return false; }

    if ((key == 'w' || key == 'W' || key == 'i') && s_idxSel > 0) {
        s_idxSel--; ensureIdxVisible(); drawIndexBrowser();
    } else if ((key == 's' || key == 'S' || key == 'k') && s_idxSel < s_idxCount - 1) {
        s_idxSel++; ensureIdxVisible(); drawIndexBrowser();
    } else if (key == '\r' || key == '\n') {
        jumpToChapter(s_idxSel);
    }

    delay(20);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

void scriptureInit(TFT_eSPI &tft) {
    s_tft         = &tft;
    s_state       = SS_BOOK_SELECT;
    s_bookSel     = 0;
    s_bookScrollOff = 0;
    s_goHome      = false;
    if (s_fileOpen) closeFile();
    s_tft->fillScreen(COL_BG);  // clear before SD access to prevent screen bleed
    checkSDCache();
    drawBookSelect();
}

bool scriptureWantsHome() { return s_goHome; }

bool scriptureLoop(TFT_eSPI &tft) {
    (void)tft;
    switch (s_state) {
        case SS_BOOK_SELECT:  return loopBookSelect();
        case SS_READING:      return loopReading();
        case SS_NO_WIFI:      return loopNoWifi();
        case SS_DOWNLOADING:  return true;
        case SS_INDEX:        return loopIndex();
    }
    return true;
}

void scriptureTrackballUp() {
    if (!s_tft) return;
    if (s_state == SS_BOOK_SELECT) {
        bookSelUp(); drawBookSelect();
    } else if (s_state == SS_INDEX) {
        if (s_idxSel > 0) { s_idxSel--; ensureIdxVisible(); drawIndexBrowser(); }
    } else if (s_state == SS_READING && s_histDepth > 0) {
        s_pageStart = s_pageHist[--s_histDepth];
        drawReadScreen();
    }
}

void scriptureTrackballDown() {
    if (!s_tft) return;
    if (s_state == SS_BOOK_SELECT) {
        bookSelDown(); drawBookSelect();
    } else if (s_state == SS_INDEX) {
        if (s_idxSel < s_idxCount - 1) { s_idxSel++; ensureIdxVisible(); drawIndexBrowser(); }
    } else if (s_state == SS_READING && s_pageNext > 0 && s_histDepth < PAGE_HIST_MAX) {
        s_pageHist[s_histDepth++] = s_pageStart;
        s_pageStart = s_pageNext;
        drawReadScreen();
    }
}

void scriptureTouchTap(uint16_t x, uint16_t y) {
    if (!s_tft) return;
    if (s_state == SS_BOOK_SELECT) {
        if (y < CONTENT_Y) return;
        int vis = (y - CONTENT_Y) / SCRIP_ROW_H;
        if (vis < 0 || vis >= SCRIP_VISIBLE) return;
        int dRow = s_bookScrollOff + vis;
        if (dRow >= totalDisplayRows()) return;
        DisplayRow dr = getDisplayRow(dRow);
        if (dr.type != ROW_BOOK) return;
        s_bookSel = dr.bookIdx;
        drawBookSelect();
        delay(80);
        if (isBookOnSD(s_bookSel)) {
            enterReading(s_bookSel);
        } else if (WiFi.status() != WL_CONNECTED) {
            s_state = SS_NO_WIFI;
            drawNoWifi();
        } else {
            s_state = SS_DOWNLOADING;
            bool ok = downloadBook(s_bookSel);
            checkSDCache();
            if (ok) enterReading(s_bookSel);
            else    { s_state = SS_BOOK_SELECT; drawBookSelect(); }
        }
    } else if (s_state == SS_INDEX) {
        if (y >= CONTENT_Y) {
            int vis = (y - CONTENT_Y) / IDX_ROW_H;
            int i   = s_idxScrollOff + vis;
            if (i >= 0 && i < s_idxCount) { s_idxSel = i; jumpToChapter(i); }
        }
    } else if (s_state == SS_READING) {
        if (y < CONTENT_Y && s_idxCount > 0) {
            // Tap the topbar/statusbar → open chapter browser
            s_idxSel = 0; s_idxScrollOff = 0;
            s_state = SS_INDEX;
            drawIndexBrowser();
        } else if (x < SCREEN_W / 2) {
            if (s_histDepth > 0) {
                s_pageStart = s_pageHist[--s_histDepth];
                drawReadScreen();
            }
        } else {
            if (s_pageNext > 0 && s_histDepth < PAGE_HIST_MAX) {
                s_pageHist[s_histDepth++] = s_pageStart;
                s_pageStart = s_pageNext;
                drawReadScreen();
            }
        }
    } else if (s_state == SS_NO_WIFI) {
        s_state = SS_BOOK_SELECT;
        drawBookSelect();
    }
}
