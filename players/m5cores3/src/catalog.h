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

namespace catalog {
// Loads + filters (drops defaultDisabled and format=="hls"). Returns false if
// the pack is missing/unparseable — treat as "needs sync".
bool load();

size_t count();
const Station& at(size_t i);
int indexOfId(const String& id);  // -1 if absent

// contentVersion of the local pack manifest ("" if none) — for the UI.
String contentVersion();
}  // namespace catalog
