#include "wh_wifi.h"

#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "secrets.h"

namespace whwifi {

bool beginConnect(WhSettings& s) {
  WiFi.mode(WIFI_STA);  // radio up even without creds — the settings scan needs it
  WiFi.setSleep(false);  // latency matters more than power for a mains radio
  if (s.ssid.isEmpty()) {
    s.ssid = WH_WIFI_SSID;
    s.pass = WH_WIFI_PASS;
    if (s.ssid != "your-ssid") whnvs::saveWifi(s.ssid, s.pass);
  }
  if (s.ssid.isEmpty() || s.ssid == "your-ssid") {
    log_e("no wifi credentials (secrets.h or the settings overlay)");
    return false;
  }
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
  return true;
}

void onLink(const WhSettings& s) {
  // Re-assert after association — modem power-save inflates RTT enough to
  // choke the (small, compile-time) lwIP TCP window below stream bitrate.
  WiFi.setSleep(false);
  log_i("wifi up: %s rssi=%d ip=%s sleep=%d", s.ssid.c_str(), WiFi.RSSI(),
        WiFi.localIP().toString().c_str(), (int)WiFi.getSleep());
}

bool syncClock(uint32_t timeoutMs) {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (time(nullptr) > 1700000000) {  // clearly post-2023 ⇒ SNTP landed
      log_i("clock synced: %lu", (unsigned long)time(nullptr));
      return true;
    }
    delay(100);
  }
  log_e("SNTP timeout — verified TLS unavailable this boot");
  return false;
}

bool joinNew(const String& ssid, const String& pass, uint32_t timeoutMs) {
  if (ssid.isEmpty()) return false;
  WiFi.disconnect();
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t deadline = millis() + timeoutMs;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
  bool ok = WiFi.status() == WL_CONNECTED;
  log_i("joinNew %s -> %s", ssid.c_str(), ok ? "ok" : "failed");
  return ok;
}

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

void maintain(const WhSettings& s) {
  static uint32_t downSince = 0, lastKick = 0;
  uint32_t now = millis();
  if (isConnected()) {
    if (downSince) {
      log_i("wifi back after %lus", (unsigned long)((now - downSince) / 1000));
      onLink(s);
    }
    downSince = 0;
    return;
  }
  if (!downSince) {
    downSince = now;
    lastKick = now;
    return;
  }
  if (s.ssid.isEmpty() || now - lastKick < WH_WIFI_REKICK_MS) return;
  lastKick = now;
  log_w("wifi down %lus — re-kicking %s", (unsigned long)((now - downSince) / 1000),
        s.ssid.c_str());
  WiFi.disconnect();  // begin() mid-attempt is rejected (ESP_ERR_WIFI_STATE)
  delay(100);
  WiFi.begin(s.ssid.c_str(), s.pass.c_str());
}

}  // namespace whwifi
