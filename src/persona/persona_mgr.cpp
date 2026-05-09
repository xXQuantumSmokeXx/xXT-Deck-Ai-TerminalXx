#include "persona_mgr.h"
#include "../config/nvs_config.h"
#include <SD.h>

#define BOARD_SDCARD_CS 39

// ── PROGMEM fallback prompts ──────────────────────────────────────────────────
static const char P1_NAME[]   PROGMEM = "ASSISTANT";
static const char P1_TITLE[]  PROGMEM = "AI Interface";
static const char P1_PROMPT[] PROGMEM = "You are a helpful AI assistant. Be concise and direct.";

// Slots 2 and 3 are blank by default — only populated from SD
static const char P2_NAME[]   PROGMEM = "PERSONA 2";
static const char P2_TITLE[]  PROGMEM = "";
static const char P3_NAME[]   PROGMEM = "PERSONA 3";
static const char P3_TITLE[]  PROGMEM = "";

// SD file paths
static const char *const SD_PATHS[PERSONA_SLOT_COUNT] = {
    "/personas/p1.txt",
    "/personas/p2.txt",
    "/personas/p3.txt",
};

// Default accent colors per slot
static const uint16_t DEFAULT_COLORS[PERSONA_SLOT_COUNT] = {
    COL_CYAN,
    COL_MAGENTA,
    COL_AMBER,
};

// ── State ─────────────────────────────────────────────────────────────────────
static PersonaDef s_slots[PERSONA_SLOT_COUNT];
static int        s_current = 0;

// ── SD loader ─────────────────────────────────────────────────────────────────
static bool loadSlotFromSD(int idx) {
    if (!SD.exists(SD_PATHS[idx])) return false;
    File f = SD.open(SD_PATHS[idx], FILE_READ);
    if (!f) return false;

    String name  = f.readStringUntil('\n'); name.trim();
    String title = f.readStringUntil('\n'); title.trim();
    String prompt = "";
    while (f.available()) prompt += (char)f.read();
    prompt.trim();
    f.close();

    // A file that only has placeholder comments counts as blank
    if (name.isEmpty() || name.startsWith("#")) return false;

    strlcpy(s_slots[idx].name,  name.c_str(),  PERSONA_NAME_LEN);
    strlcpy(s_slots[idx].title, title.c_str(), PERSONA_TITLE_LEN);
    s_slots[idx].systemPrompt = prompt;
    s_slots[idx].color        = DEFAULT_COLORS[idx];
    s_slots[idx].loaded       = true;
    return true;
}

// ── Init ──────────────────────────────────────────────────────────────────────
void personaMgrInit() {
    // Zero all slots
    for (int i = 0; i < PERSONA_SLOT_COUNT; i++) {
        memset(s_slots[i].name,  0, PERSONA_NAME_LEN);
        memset(s_slots[i].title, 0, PERSONA_TITLE_LEN);
        s_slots[i].systemPrompt = "";
        s_slots[i].color        = DEFAULT_COLORS[i];
        s_slots[i].loaded       = false;
    }

    bool sdOk = SD.begin(BOARD_SDCARD_CS);

    // Slot 0 — always has the built-in fallback
    if (!sdOk || !loadSlotFromSD(0)) {
        strlcpy(s_slots[0].name,  P1_NAME,   PERSONA_NAME_LEN);
        strlcpy(s_slots[0].title, P1_TITLE,  PERSONA_TITLE_LEN);
        s_slots[0].systemPrompt = String(P1_PROMPT);
        s_slots[0].color        = DEFAULT_COLORS[0];
        s_slots[0].loaded       = true;
    }

    // Slots 1 and 2 — SD only; leave unloaded if files missing/blank
    if (sdOk) {
        loadSlotFromSD(1);
        loadSlotFromSD(2);
    }

    if (sdOk) SD.end();

    personaMgrLoad();
    // Clamp to a loaded slot
    if (!s_slots[s_current].loaded) s_current = 0;
}

// ── Accessors ─────────────────────────────────────────────────────────────────
PersonaDef *personaMgrGet()         { return &s_slots[s_current]; }
int         personaMgrCurrentSlot() { return s_current; }

int personaMgrLoadedCount() {
    int n = 0;
    for (int i = 0; i < PERSONA_SLOT_COUNT; i++) if (s_slots[i].loaded) n++;
    return n;
}

bool personaMgrNext() {
    if (personaMgrLoadedCount() <= 1) return false;
    int next = (s_current + 1) % PERSONA_SLOT_COUNT;
    // Skip unloaded slots
    int tries = 0;
    while (!s_slots[next].loaded && tries < PERSONA_SLOT_COUNT) {
        next = (next + 1) % PERSONA_SLOT_COUNT;
        tries++;
    }
    if (next == s_current) return false;
    s_current = next;
    personaMgrSave();
    return true;
}

bool personaMgrSet(int slot) {
    if (slot < 0 || slot >= PERSONA_SLOT_COUNT) return false;
    if (!s_slots[slot].loaded) return false;
    if (slot == s_current) return false;
    s_current = slot;
    personaMgrSave();
    return true;
}

void personaMgrSave() {
    nvsPutInt("persona_slot", s_current);
}

void personaMgrLoad() {
    s_current = nvsGetInt("persona_slot", 0);
    if (s_current < 0 || s_current >= PERSONA_SLOT_COUNT) s_current = 0;
}
