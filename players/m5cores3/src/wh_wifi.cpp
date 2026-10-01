#include "wh_wifi.h"

#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "secrets.h"

namespace {

// Switch the STA to (ssid, pass) even while an association attempt is in
// flight. A plain disconnect()+begin() loses that race: while the stored
// network is out of range the stack keeps re-trying it, and the driver
// rejects the new config mid-attempt ("sta is connecting, cannot set config",
// ESP_ERR_WIFI_STATE) — begin() fails and the OLD network keeps retrying.
// Measured on the Fire moved to a new site: wifi-join never took. Stop the
// auto-retry first, then re-issue begin() until the driver accepts it.
bool restartAssociation(const String& ssid, const String& pass) {
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  bool accepted = false;
  uint32_t deadline = millis() + 4000;
  while (!accepted && millis() < deadline) {
    delay(150);
    accepted = WiFi.begin(ssid.c_str(), pass.c_str()) != WL_CONNECT_FAILED;
  }
  WiFi.setAutoReconnect(true);  // the stack retries the NEW network from here on
  if (!accepted) log_e("wifi: driver kept rejecting the new config for %s", ssid.c_str());
  return accepted;
}

}  // namespace

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
  restartAssociation(s.ssid, s.pass);  // also re-kicks after a failed join
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
  if (!restartAssociation(ssid, pass)) return false;
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
  restartAssociation(s.ssid, s.pass);  // begin() mid-attempt is rejected otherwise
}

}  // namespace whwifi
