// WaveHopper CoreS3 firmware — skeleton.
//
// Boots the display, mounts LittleFS, and reports the seeded content version.
// No player logic yet. The idle Audio object is deliberate: it forces the full
// ESP32-audioI2S dependency to compile and link against the pinned core, so
// toolchain drift is caught by `pio run` long before the audio work starts.
// Read CLAUDE.md (I2S/AW88298 handoff) before making it play anything.

#include <M5Unified.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Audio.h>

#include "config.h"

Audio audio;

static String contentVersion = "(no pack)";

static void readContentVersion() {
  File f = LittleFS.open(WH_FS_CONTENT_DIR "/manifest.json", "r");
  if (!f) return;
  JsonDocument filter;
  filter["contentVersion"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, f, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
    const char* version = doc["contentVersion"];
    if (version) contentVersion = version;
  }
  f.close();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  if (LittleFS.begin(true)) {  // format on first boot; empty FS = full sync needed
    readContentVersion();
  } else {
    contentVersion = "(fs error)";
  }

  M5.Display.setTextSize(2);
  M5.Display.println("WaveHopper");
  M5.Display.printf("fw %s (build %d)\n", WH_FW_VERSION, WH_FW_BUILD);
  M5.Display.printf("content %.12s\n", contentVersion.c_str());
  M5.Display.printf("host %s\n", WH_CONTENT_HOST);

  // TODO(player): Wi-Fi provisioning via NVS, content sync (CONTENT-API.md
  // algorithm), firmware OTA check, station list UI, playback.
}

void loop() {
  M5.update();
  delay(50);
}
