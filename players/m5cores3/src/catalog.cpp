#include "catalog.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

namespace {
std::vector<Station> g_stations;
String g_contentVersion;

uint16_t hexColorTo565(const char* hex) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0xFFFF;
  uint32_t rgb = strtoul(hex + 1, nullptr, 16);
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}
}  // namespace

namespace catalog {

bool load() {
  g_stations.clear();

  File f = LittleFS.open(WH_FS_CONTENT_DIR "/stations.json", "r");
  if (!f) {
    log_e("no stations.json in local pack");
    return false;
  }

  // ~10 KB array; filter keeps memory flat if the catalog grows.
  JsonDocument filter;
  JsonObject sf = filter.add<JsonObject>();
  sf["id"] = true;
  sf["station"] = true;
  sf["channel"] = true;
  sf["city"] = true;
  sf["url"] = true;
  sf["format"] = true;
  sf["color"] = true;
  sf["icon"] = true;
  sf["defaultDisabled"] = true;
  sf["nowPlaying"]["type"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  if (err != DeserializationError::Ok) {
    log_e("stations.json parse failed: %s", err.c_str());
    return false;
  }

  for (JsonObject o : doc.as<JsonArray>()) {
    if (o["defaultDisabled"] | false) continue;
    const char* format = o["format"] | "";
    if (strcmp(format, "hls") == 0) continue;  // unplayable on this hardware (v1)

    Station s;
    s.id = (const char*)(o["id"] | "");
    s.name = (const char*)(o["station"] | "");
    s.channel = (const char*)(o["channel"] | "");
    s.city = (const char*)(o["city"] | "");
    s.url = (const char*)(o["url"] | "");
    s.color565 = hexColorTo565(o["color"] | "");
    const char* icon = o["icon"] | "";
    if (icon[0]) s.iconPath = String(WH_FS_CONTENT_DIR "/") + icon;
    s.npType = (const char*)(o["nowPlaying"]["type"] | "");
    s.pollable = !s.npType.isEmpty() && s.npType != "none" && s.npType != "hls-id3";
    if (s.id.isEmpty() || s.url.isEmpty()) continue;
    g_stations.push_back(std::move(s));
  }

  // contentVersion from the local manifest (display/logging only).
  File mf = LittleFS.open(WH_FS_CONTENT_DIR "/manifest.json", "r");
  if (mf) {
    JsonDocument mfilter;
    mfilter["contentVersion"] = true;
    JsonDocument mdoc;
    if (deserializeJson(mdoc, mf, DeserializationOption::Filter(mfilter)) ==
        DeserializationError::Ok) {
      g_contentVersion = (const char*)(mdoc["contentVersion"] | "");
    }
    mf.close();
  }

  log_i("catalog: %u playable stations (content %.12s)", g_stations.size(),
        g_contentVersion.c_str());
  return !g_stations.empty();
}

size_t count() { return g_stations.size(); }

const Station& at(size_t i) { return g_stations[i]; }

int indexOfId(const String& id) {
  for (size_t i = 0; i < g_stations.size(); ++i) {
    if (g_stations[i].id == id) return (int)i;
  }
  return -1;
}

String contentVersion() { return g_contentVersion; }

}  // namespace catalog
