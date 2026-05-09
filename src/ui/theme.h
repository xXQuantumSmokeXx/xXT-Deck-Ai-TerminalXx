#pragma once
#include <TFT_eSPI.h>

// ── Screen dimensions ─────────────────────────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  240

// ── Color palette (RGB565) ────────────────────────────────────────────────────
#define COL_BG        0x0000  // black
#define COL_CYAN      0x07FF  // bright cyan
#define COL_MAGENTA   0xF815  // #FF00AA — AI chat
#define COL_AMBER     0xFD40  // #FFAA00 — solar / warnings
#define COL_PURPLE    0xAC5F  // #AA88FF — mesh
#define COL_GOLD      0xFE80  // #FFCC00 — BTC
#define COL_BLUE      0x065F  // #00CCFF — system
#define COL_WHITE     0xFFFF
#define COL_RED       0xF800  // #FF0000 — storm alerts, southward Bz
#define COL_GREY_DIM  0x01EF  // dim cyan (was dark grey)
#define COL_GREY_MID  0x03EF  // medium cyan (was light grey)
#define COL_INPUT_BG  0x0104  // very dark cyan (was near-black grey)
#define COL_ORANGE    0x07FF  // bright cyan (was orange — prompts)
#define COL_GREEN     0x07C0  // #00F800 — world/intel tile

// Chat agent colors
#define COL_YOU   COL_CYAN
#define COL_SYS   COL_GREY_MID

// ── Typography ────────────────────────────────────────────────────────────────
#define FONT_SMALL  1   // 8px  — status text, labels
#define FONT_MED    2   // 16px — chat text, tiles
#define FONT_LARGE  4   // 26px — big numbers

// ── Layout constants ──────────────────────────────────────────────────────────
#define TOPBAR_H     20
#define STATUSBAR_H  14
#define BOTTOMBAR_H  14
#define CONTENT_Y    (TOPBAR_H + STATUSBAR_H)
#define CONTENT_H    (SCREEN_H - CONTENT_Y - BOTTOMBAR_H)

// ── Tile grid ─────────────────────────────────────────────────────────────────
#define TILE_COLS    4
#define TILE_ROWS    2
#define TILE_W       ((SCREEN_W) / TILE_COLS)         // 80px
#define TILE_H       (CONTENT_H / TILE_ROWS)           // ~96px
#define TILE_PAD     4
