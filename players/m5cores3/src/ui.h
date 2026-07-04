// Screen rendering: boot status lines, canvas-based station card (icon,
// marquee title), transient station-list toast on change, volume overlay.
#pragma once

#include <Arduino.h>

#include "now_playing.h"
#include "player.h"

namespace ui {
void begin(uint8_t brightness);

// Boot status screen (scrolling text lines while the sequencer runs).
void bootLine(const char* fmt, ...);

// Rebuild + push the card. np may carry empty strings (falls back to ICY
// title from the snapshot, then a generic on-air marker).
void render(const PlayerSnapshot& snap, const NowPlaying& np);

// Transient overlays (tick() restores the card when they expire).
void stationToast(int currentIndex);  // list of neighbors, ~2 s
void volumeOverlay(uint8_t vol);

// Modal settings overlay (BtnB hold). While open, main routes taps to
// settingsTouch() and applies the returned action.
enum class SettingsAction { None, Close, CloseAndReboot, ConnectWifi };
bool settingsOpen();
void settingsShow(AudioOutSetting audioOut, uint8_t brightness);
SettingsAction settingsTouch(int x, int y);
bool settingsScroll(int rows);        // drag-scroll the active list page
uint8_t settingsBrightness();
String settingsWifiSsid();            // credentials entered on the keyboard
String settingsWifiPassword();
void settingsWifiResult(bool ok);     // main calls after a connect attempt

// Thin cushion-health strip at the bottom edge (muted green/amber/red fill
// proportional to buffered/target). Call ~1 Hz while playing.
void bufferGauge(uint32_t buffered, uint32_t target);

// Small 4-bar RSSI meter, top-right. Call ~1 Hz alongside the gauge.
void wifiMeter(int rssi);

// Timers: marquee scroll + overlay expiry. Call every loop.
void tick();
}  // namespace ui
