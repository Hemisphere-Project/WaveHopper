// Screen rendering: boot status lines, the full-screen station card, and the
// transient volume overlay. M1 draws directly; M2 moves to canvases + toast
// list + marquee + icons.
#pragma once

#include <Arduino.h>

#include "player.h"

namespace ui {
void begin(uint8_t brightness);

// Boot status screen (scrolling text lines while the sequencer runs).
void bootLine(const char* fmt, ...);

// Redraw the card for the given snapshot (station looked up via catalog).
void render(const PlayerSnapshot& snap);

// Show volume bar for ~1.5 s (tick() restores the card afterwards).
void volumeOverlay(uint8_t vol);

// Timers (overlay expiry). Call every loop.
void tick();
}  // namespace ui
