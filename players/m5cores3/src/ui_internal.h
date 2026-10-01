// Shared drawing vocabulary for the ui*.cpp files (palette, fonts, rows,
// soft-key bar). Fonts are DEFINED once in ui.cpp: include/font_vt323.h holds
// `static` (and non-const) glyph tables — including it in a second
// translation unit would duplicate ~6.7 KB of them into internal DRAM.
#pragma once

#include <M5Unified.h>

namespace ui::detail {

constexpr int W = 320, H = 240;

// Web player's default (Dark) skin palette — style.css :root.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG = rgb565(0x0a, 0x0a, 0x0a);      // --bg
constexpr uint16_t COL_FG = rgb565(0xe8, 0xe8, 0xe8);      // --fg
constexpr uint16_t COL_DIM = rgb565(0x66, 0x66, 0x66);     // muted labels/hints
constexpr uint16_t COL_LINE = rgb565(0x2a, 0x2a, 0x2a);    // borders/dividers
constexpr uint16_t COL_ACCENT = rgb565(0xff, 0xf2, 0x05);  // --accent (yellow) for chrome
constexpr uint16_t COL_PANEL = rgb565(0x16, 0x16, 0x16);   // row/panel fill
constexpr uint16_t COL_OK = rgb565(0x2a, 0xeb, 0x62);      // checkbox / success
constexpr uint16_t COL_ERR = rgb565(0xeb, 0x2a, 0x2a);     // failure notes

extern const lgfx::GFXfont& F_SMALL;  // VT323 20
extern const lgfx::GFXfont& F_MED;    // VT323 24 — metadata, rows
extern const lgfx::GFXfont& F_BIG;    // VT323 34 — station name, headers

// Page header: accent title + underline. Returns the y below it for content.
int drawHeader(LovyanGFX& d, const char* title);

// A full-width row with a label and an optional right-aligned value.
void drawRow(LovyanGFX& d, int y, const char* label, const char* value);

// Modal settings UI owns the screen (card pushes, overlays, marquees pause).
extern bool g_settingsOpen;
// Push the station card back after a modal closes (no-op before the first
// render — e.g. settings opened during the boot wifi wait).
void restoreCard();

}  // namespace ui::detail
