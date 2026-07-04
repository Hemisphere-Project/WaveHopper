#include "catalog.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

namespace {
constexpr const char* kPrefsPath = "/prefs.json";

std::vector<Station> g_stations;
String g_contentVersion;

uint16_t hexColorTo565(const char* hex) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return 0xFFFF;
  uint32_t rgb = strtoul(hex + 1, nullptr, 16);
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

// User visibility overrides, relative to the pack's defaultDisabled defaults:
// "disabled" hides default-visible stations, "enabled" shows default-hidden.
struct Prefs {
  std::vector<String> disabled;
  std::vector<String> enabled;
};

bool inList(const std::vector<String>& v, const String& id) {
  for (auto& e : v)
    if (e == id) return true;
  return false;
}

Prefs loadPrefs() {
  Prefs prefs;
  File f = LittleFS.open(kPrefsPath, "r");
  if (!f) return prefs;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    for (JsonVariant v : doc["disabled"].as<JsonArray>()) prefs.disabled.push_back((const char*)v);
    for (JsonVariant v : doc["enabled"].as<JsonArray>()) prefs.enabled.push_back((const char*)v);
  }
  f.close();
  return prefs;
}

void savePrefs(const Prefs& prefs) {
  JsonDocument doc;
  JsonArray d = doc["disabled"].to<JsonArray>();
  for (auto& id : prefs.disabled) d.add(id);
  JsonArray e = doc["enabled"].to<JsonArray>();
  for (auto& id : prefs.enabled) e.add(id);
  File f = LittleFS.open(kPrefsPath, "w");
  if (!f) {
    log_e("prefs save failed");
    return;
  }
  serializeJson(doc, f);
  f.close();
}

bool visibleFor(bool defaultDisabled, const Prefs& prefs, const String& id) {
  if (defaultDisabled) return inList(prefs.enabled, id);
  return !inList(prefs.disabled, id);
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

  Prefs prefs = loadPrefs();
  for (JsonObject o : doc.as<JsonArray>()) {
    // No format filtering here: the m5 pack is device-specific and the build
    // only includes playable streams (HLS only via a verified m5Url).
    String id = (const char*)(o["id"] | "");
    if (!visibleFor(o["defaultDisabled"] | false, prefs, id)) continue;

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
    s.isHls = strcmp(o["format"] | "", "hls") == 0;  // build ships only playable HLS (m5Url)
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

std::vector<StationMeta> allMeta() {
  std::vector<StationMeta> metas;
  File f = LittleFS.open(WH_FS_CONTENT_DIR "/stations.json", "r");
  if (!f) return metas;
  JsonDocument filter;
  JsonObject sf = filter.add<JsonObject>();
  sf["id"] = true;
  sf["station"] = true;
  sf["channel"] = true;
  sf["format"] = true;
  sf["defaultDisabled"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f, DeserializationOption::Filter(filter));
  f.close();
  if (err != DeserializationError::Ok) return metas;

  Prefs prefs = loadPrefs();
  for (JsonObject o : doc.as<JsonArray>()) {
    StationMeta m;
    m.id = (const char*)(o["id"] | "");
    String ch = (const char*)(o["channel"] | "");
    m.label = String((const char*)(o["station"] | ""));
    if (!ch.isEmpty() && ch != "main") m.label += " " + ch;
    m.visible = visibleFor(o["defaultDisabled"] | false, prefs, m.id);
    if (!m.id.isEmpty()) metas.push_back(std::move(m));
  }
  return metas;
}

void toggleUserVisible(const String& id) {
  // Determine the station's default from the pack, then flip relative to it.
  bool defaultDisabled = false;
  {
    File f = LittleFS.open(WH_FS_CONTENT_DIR "/stations.json", "r");
    if (!f) return;
    JsonDocument filter;
    JsonObject sf = filter.add<JsonObject>();
    sf["id"] = true;
    sf["defaultDisabled"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, f, DeserializationOption::Filter(filter)) !=
        DeserializationError::Ok) {
      f.close();
      return;
    }
    f.close();
    for (JsonObject o : doc.as<JsonArray>()) {
      if (id == (const char*)(o["id"] | "")) {
        defaultDisabled = o["defaultDisabled"] | false;
        break;
      }
    }
  }
  Prefs prefs = loadPrefs();
  auto& overrides = defaultDisabled ? prefs.enabled : prefs.disabled;
  bool removed = false;
  for (size_t i = 0; i < overrides.size(); ++i) {
    if (overrides[i] == id) {
      overrides.erase(overrides.begin() + i);
      removed = true;
      break;
    }
  }
  if (!removed) overrides.push_back(id);
  savePrefs(prefs);
}

}  // namespace catalog
