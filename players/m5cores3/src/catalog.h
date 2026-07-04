// Station catalog, loaded from the synced content pack on LittleFS.
// Pack format: docs/CONTENT-API.md (m5 stations.json — icon paths are
// pack-relative, `format` drives playability filtering).
#pragma once

#include <Arduino.h>

#include <vector>

struct Station {
  String id;
  String name;      // "station" field
  String channel;   // may be empty
  String city;
  String url;
  String iconPath;  // absolute LittleFS path, empty if none
  String npType;    // nowPlaying.type or empty
  uint16_t color565 = 0xFFFF;
  bool pollable = false;  // npType exists and isn't "none"/"hls-id3"
};

// Settings support: every selectable station in the pack (hls excluded),
// with its current visibility (defaults + user prefs from /prefs.json).
struct StationMeta {
  String id;
  String label;
  bool visible;
};

namespace catalog {
// Loads + filters (hls always dropped; visibility = defaultDisabled defaults
// overridden by user prefs). Returns false if the pack is missing/unparseable
// — treat as "needs sync".
bool load();

// Full selectable list for the settings page + a visibility toggle (persists
// to /prefs.json; catalog indexes change on next load — reboot to apply).
std::vector<StationMeta> allMeta();
void toggleUserVisible(const String& id);

size_t count();
const Station& at(size_t i);
int indexOfId(const String& id);  // -1 if absent

// contentVersion of the local pack manifest ("" if none) — for the UI.
String contentVersion();
}  // namespace catalog
