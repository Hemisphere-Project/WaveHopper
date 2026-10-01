// Screen rendering: boot status lines, canvas-based station card (icon,
// marquee title), transient station-list toast on change, volume overlay.
#pragma once

#include <Arduino.h>

#include "now_playing.h"
#include "player.h"

namespace ui {
void begin(uint8_t brightness);

// (Re)draw the boot splash + status scroll region. begin() draws it once;
// main re-calls it when the settings overlay closes during the boot wifi wait.
void bootScreen();

// True when (x,y) lands on the boot header's settings gear (top-right).
bool bootGearHit(int x, int y);


// Boot status screen (scrolling text lines while the sequencer runs).
void bootLine(const char* fmt, ...);

// Rebuild + push the card. np may carry empty strings (falls back to ICY
// title from the snapshot, then a generic on-air marker).
// Decode every station icon once into PSRAM (call after catalog::load, while
// internal heap is plentiful — before any stream starts).
void preloadIcons();
void render(const PlayerSnapshot& snap, const NowPlaying& np);

// Transient overlays (tick() restores the card when they expire).
void stationToast(int currentIndex);  // list of neighbors, ~2 s
// holdMs: how long the bar stays up (button devices hold it for the whole
// volume mode and draw its soft keys). dismissOverlay() ends it early.
void volumeOverlay(uint8_t vol, uint32_t holdMs = 1500);
void dismissOverlay();

// Modal settings UI (hold BtnB; touch: also hold the card / boot gear).
// Touch boards: main routes taps to settingsTouch(). Button boards: main
// routes A/B/C to settingsKey() — a list menu. Both return an action.
enum class SettingsAction {
  None,
  Close,
  CloseAndReboot,
  ConnectWifi,  // touch keyboard: join settingsWifiSsid()/Password()
  PhoneSetup,   // open the Wi-Fi setup portal (phone + QR codes)
  ForgetWifi,   // clear stored credentials (next boot opens the portal)
};
bool settingsOpen();
void settingsShow(AudioOutSetting audioOut, uint8_t brightness);
SettingsAction settingsTouch(int x, int y);
bool settingsScroll(int rows);        // drag-scroll the active list page
enum class MenuKey : uint8_t { Up, Down, Select, Back };
SettingsAction settingsKey(MenuKey k);
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
