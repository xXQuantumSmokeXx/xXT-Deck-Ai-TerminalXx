#pragma once
#include <Arduino.h>
#include "../ui/theme.h"

#define PERSONA_SLOT_COUNT  3
#define PERSONA_NAME_LEN    32
#define PERSONA_TITLE_LEN   48

struct PersonaDef {
    char     name[PERSONA_NAME_LEN];
    char     title[PERSONA_TITLE_LEN];
    uint16_t color;
    String   systemPrompt;
    bool     loaded;
};

// Call once at boot (after SD.begin elsewhere, persona_mgr handles its own SD open)
void         personaMgrInit();

// Active persona
PersonaDef  *personaMgrGet();
int          personaMgrCurrentSlot();
int          personaMgrLoadedCount();

// Cycle to next populated slot. Returns true if slot actually changed.
bool         personaMgrNext();

// Jump to a specific slot (0-based). Returns true if valid and changed.
bool         personaMgrSet(int slot);

// Persist/restore active slot index via NVS
void         personaMgrSave();
void         personaMgrLoad();
