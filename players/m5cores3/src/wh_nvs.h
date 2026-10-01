// Persistent device settings (NVS via Preferences, namespace "wh").
// Key schema is documented in include/config.h.
#pragma once

#include <Arduino.h>

enum class AudioOutSetting : uint8_t { Auto = 0, Internal = 1, Rca = 2, ModuleAudio = 3 };

struct WhSettings {
  String ssid;
  String pass;
  String lastStation;
  uint8_t volume = 21;       // 0..21 (ESP32-audioI2S scale), default = max
  AudioOutSetting audioOut = AudioOutSetting::Auto;
  uint8_t brightness = 200;  // 0..255
  // Credentials were explicitly forgotten (ssid key present but empty): no
  // fallback to the compiled-in secrets.h network — the portal takes over.
  bool wifiForgotten = false;
};

namespace whnvs {
void load(WhSettings& s);
void saveWifi(const String& ssid, const String& pass);
void saveLastStation(const String& id);
void saveVolume(uint8_t v);
void saveAudioOut(AudioOutSetting a);
void saveBrightness(uint8_t b);
// Clear the stored network (keeps an empty ssid key = wifiForgotten).
void forgetWifi();
// One-shot "open the Wi-Fi setup portal on next boot" (settings → phone
// setup while playing: a clean boot runs the portal before the player).
void setPortalOnBoot();
bool takePortalOnBoot();  // reads AND clears it
}  // namespace whnvs
