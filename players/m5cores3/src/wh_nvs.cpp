#include "wh_nvs.h"

#include <Preferences.h>

namespace {
constexpr const char* kNamespace = "wh";

// Short-lived handle per operation: writes are rare (settings changes), and
// holding NVS open buys nothing.
template <typename Fn>
void withPrefs(bool readOnly, Fn fn) {
  Preferences prefs;
  if (prefs.begin(kNamespace, readOnly)) {
    fn(prefs);
    prefs.end();
  }
}
}  // namespace

namespace whnvs {

void load(WhSettings& s) {
  withPrefs(true, [&](Preferences& p) {
    s.ssid = p.getString("ssid", "");
    s.pass = p.getString("pass", "");
    s.lastStation = p.getString("last_st", "");
    s.volume = p.getUChar("vol", 12);
    uint8_t aout = p.getUChar("aout", 0);
    s.audioOut = aout <= 3 ? static_cast<AudioOutSetting>(aout) : AudioOutSetting::Auto;
    s.brightness = p.getUChar("bright", 200);
  });
}

void saveWifi(const String& ssid, const String& pass) {
  withPrefs(false, [&](Preferences& p) {
    p.putString("ssid", ssid);
    p.putString("pass", pass);
  });
}

void saveLastStation(const String& id) {
  withPrefs(false, [&](Preferences& p) { p.putString("last_st", id); });
}

void saveVolume(uint8_t v) {
  withPrefs(false, [&](Preferences& p) { p.putUChar("vol", v); });
}

void saveAudioOut(AudioOutSetting a) {
  withPrefs(false, [&](Preferences& p) { p.putUChar("aout", static_cast<uint8_t>(a)); });
}

void saveBrightness(uint8_t b) {
  withPrefs(false, [&](Preferences& p) { p.putUChar("bright", b); });
}

}  // namespace whnvs
