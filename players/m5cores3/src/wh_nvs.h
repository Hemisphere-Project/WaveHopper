// Persistent device settings (NVS via Preferences, namespace "wh").
// Key schema is documented in include/config.h.
#pragma once

#include <Arduino.h>

enum class AudioOutSetting : uint8_t { Auto = 0, Internal = 1, Rca = 2, ModuleAudio = 3 };

struct WhSettings {
  String ssid;
  String pass;
  String lastStation;
  uint8_t volume = 12;       // 0..21 (ESP32-audioI2S scale)
  AudioOutSetting audioOut = AudioOutSetting::Auto;
  uint8_t brightness = 200;  // 0..255
};

namespace whnvs {
void load(WhSettings& s);
void saveWifi(const String& ssid, const String& pass);
void saveLastStation(const String& id);
void saveVolume(uint8_t v);
void saveAudioOut(AudioOutSetting a);
void saveBrightness(uint8_t b);
}  // namespace whnvs
