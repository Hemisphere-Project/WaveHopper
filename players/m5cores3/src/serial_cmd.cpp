#include "serial_cmd.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>

#include "catalog.h"
#include "config.h"
#include "player.h"
#include "wh_nvs.h"
#include "wh_wifi.h"

namespace {

char g_line[96];
size_t g_len = 0;
bool g_ready = false;  // player + catalog up (setReady) — else boot-safe only

// Bench Wi-Fi provisioning: SSID and password are set by separate commands so
// either may contain spaces (each takes the rest of its line verbatim).
String g_wifiSsid, g_wifiPass;

void reply(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print('@');
  Serial.println(buf);
}

// "<index>" or "<station id>" → catalog index, -1 if neither.
int resolveStation(const char* arg) {
  if (!*arg) return -1;
  char* end;
  long v = strtol(arg, &end, 10);
  if (*end == '\0') return (v >= 0 && v < (long)catalog::count()) ? (int)v : -1;
  return catalog::indexOfId(String(arg));
}

void status() {
  PlayerSnapshot s = player::snapshot();
  const char* id = (s.stationIndex >= 0 && s.stationIndex < (int)catalog::count())
                       ? catalog::at(s.stationIndex).id.c_str()
                       : "-";
  reply("status state=%s idx=%d id=%s buf=%lu target=%lu vol=%u wifi=%d rssi=%d "
        "heap=%lu maxblk=%lu psram=%lu up=%lus fw=%s+%d",
        player::stateName(s.state), s.stationIndex, id, (unsigned long)s.buffered,
        (unsigned long)s.bufferTarget, s.volume, WiFi.status() == WL_CONNECTED ? 1 : 0,
        WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
        (unsigned long)ESP.getFreePsram(), (unsigned long)(millis() / 1000), WH_FW_VERSION,
        WH_FW_BUILD);
}

void run(char* line) {
  char* arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  } else {
    arg = line + strlen(line);
  }
  int n = (int)catalog::count();
  PlayerSnapshot s = player::snapshot();
  int cur = s.stationIndex < 0 ? 0 : s.stationIndex;

  if (!strcmp(line, "help")) {
    reply("help status | list | tune <idx|id> | next | prev | retune | vol <0-21> | "
          "dns <host> | net | wifi-drop | wifi-ssid <ssid> | wifi-pass <pass> | "
          "wifi-join | portal | reboot");
  } else if (!strcmp(line, "wifi-ssid")) {
    if (!*arg || strlen(arg) > 32) return reply("err wifi-ssid <1..32 chars>");
    g_wifiSsid = arg;
    reply("ok wifi-ssid %s", arg);
  } else if (!strcmp(line, "wifi-pass")) {
    // Empty = open network. Never echoed back (the log may be shared).
    if (strlen(arg) > 63) return reply("err wifi-pass <0..63 chars>");
    g_wifiPass = arg;
    reply("ok wifi-pass (%u chars)", (unsigned)strlen(arg));
  } else if (!strcmp(line, "wifi-join")) {
    // Same contract as the settings join: verify first, persist + reboot only
    // on success (the boot path brings everything up cleanly on the new link).
    if (g_wifiSsid.isEmpty()) return reply("err wifi-ssid first");
    // Progress goes to the log only — a host waits for the ok/err '@' reply.
    Serial.printf("joining %s ...\n", g_wifiSsid.c_str());
    if (whwifi::joinNew(g_wifiSsid, g_wifiPass, 15000)) {
      whnvs::saveWifi(g_wifiSsid, g_wifiPass);
      reply("ok wifi-join %s — saved, rebooting", g_wifiSsid.c_str());
      Serial.flush();
      delay(50);
      ESP.restart();
    }
    reply("err wifi-join %s failed — back to the stored network", g_wifiSsid.c_str());
    WhSettings stored;
    whnvs::load(stored);
    whwifi::beginConnect(stored);
  } else if (!strcmp(line, "portal")) {
    // Same path as settings → phone setup while playing: clean boot → portal.
    whnvs::setPortalOnBoot();
    reply("ok portal — rebooting into wifi setup");
    Serial.flush();
    delay(50);
    ESP.restart();
  } else if (!g_ready && !strcmp(line, "status")) {
    reply("status state=booting wifi=%d rssi=%d heap=%lu maxblk=%lu psram=%lu up=%lus "
          "fw=%s+%d board=%s",
          WiFi.status() == WL_CONNECTED ? 1 : 0, WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(),
          (unsigned long)ESP.getMaxAllocHeap(), (unsigned long)ESP.getFreePsram(),
          (unsigned long)(millis() / 1000), WH_FW_VERSION, WH_FW_BUILD, WH_BOARD_ID);
  } else if (!g_ready && strcmp(line, "net") && strcmp(line, "reboot") && *line) {
    reply("err '%s' not available while booting", line);
  } else if (!strcmp(line, "status")) {
    status();
  } else if (!strcmp(line, "list")) {
    for (int i = 0; i < n; i++)
      reply("list %d %s %s", i, catalog::at(i).id.c_str(), catalog::at(i).url.c_str());
  } else if (!strcmp(line, "tune")) {
    int idx = resolveStation(arg);
    if (idx < 0) return reply("err unknown station '%s'", arg);
    player::tuneTo(idx);
    reply("ok tune %d", idx);
  } else if (!strcmp(line, "next") || !strcmp(line, "prev")) {
    if (!n) return reply("err empty catalog");
    int idx = ((cur + (line[0] == 'n' ? 1 : -1)) % n + n) % n;
    player::tuneTo(idx);
    reply("ok tune %d", idx);
  } else if (!strcmp(line, "retune")) {
    player::retune();
    reply("ok retune %d", cur);
  } else if (!strcmp(line, "vol")) {
    char* end;
    long v = strtol(arg, &end, 10);
    if (!*arg || *end || v < 0 || v > 21) return reply("err vol 0..21");
    player::setVolume((uint8_t)v);
    reply("ok vol %ld", v);
  } else if (!strcmp(line, "dns")) {
    if (!*arg) return reply("err dns <host>");
    // Runs on the UI loop: offline, each lookup blocks ~7 s (and starves the
    // wifi watchdog on the same loop).
    if (WiFi.status() != WL_CONNECTED) return reply("err dns: wifi down");
    IPAddress ip;
    uint32_t t0 = millis();
    bool ok = WiFi.hostByName(arg, ip) == 1;
    reply("dns %s -> %s in %lums", arg, ok ? ip.toString().c_str() : "FAIL",
          (unsigned long)(millis() - t0));
  } else if (!strcmp(line, "net")) {
    // lwIP's live resolver list (DHCP and IPv6 RA/RDNSS both write it).
    char dnsbuf[120] = "";
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
      const ip_addr_t* a = dns_getserver(i);
      char one[48];
      ipaddr_ntoa_r(a, one, sizeof(one));
      strlcat(dnsbuf, i ? " " : "", sizeof(dnsbuf));
      strlcat(dnsbuf, one, sizeof(dnsbuf));
    }
    reply("net ip=%s gw=%s dns=[%s] ip6ll=%s ip6g=%s up=%lus",
          WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), dnsbuf,
          WiFi.linkLocalIPv6().toString().c_str(), WiFi.globalIPv6().toString().c_str(),
          (unsigned long)(millis() / 1000));
  } else if (!strcmp(line, "wifi-drop")) {
    reply("ok wifi-drop");  // exercises the runtime watchdog (whwifi::maintain)
    WiFi.disconnect();
  } else if (!strcmp(line, "reboot")) {
    reply("ok reboot");
    Serial.flush();
    delay(50);
    ESP.restart();
  } else if (*line) {
    reply("err unknown command '%s' (try help)", line);
  }
}

}  // namespace

namespace serial_cmd {

void setReady() { g_ready = true; }

void poll() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      g_line[g_len] = '\0';
      g_len = 0;
      run(g_line);
      continue;
    }
    if (g_len < sizeof(g_line) - 1) g_line[g_len++] = (char)c;
  }
}

}  // namespace serial_cmd
