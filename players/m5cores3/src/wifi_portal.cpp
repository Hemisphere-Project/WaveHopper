#include "wifi_portal.h"

#include <DNSServer.h>
#include <M5Unified.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include <vector>

#include "config.h"
#include "serial_cmd.h"
#include "ui_internal.h"
#include "wh_nvs.h"

namespace {

using namespace ui::detail;

constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr int kCancelY = 228;  // touch boards: centre of the "cancel" target
constexpr size_t kMaxNets = 24;
const IPAddress kApIp(192, 168, 4, 1);

struct Net {
  String ssid;
  int rssi;
  bool secure;
};

enum class JoinState : uint8_t { Idle, Pending, Joining, Ok, Fail };

std::vector<Net> g_nets;
JoinState g_state = JoinState::Idle;
String g_joinSsid, g_joinPass;
String g_apSsid, g_apPass;
WebServer* g_server = nullptr;  // per session: the heap is only spent while open
DNSServer* g_dns = nullptr;

// ---------------------------------------------------------------- screen ---

void drawStatus(const char* msg, uint16_t col) {
  auto& d = M5.Display;
  d.fillRect(150, 8, W - 160, 30, COL_BG);
  d.setFont(&F_SMALL);
  d.setTextDatum(top_right);
  d.setTextColor(col, COL_BG);
  String s = msg;
  while (d.textWidth(s.c_str()) > W - 166 && s.length() > 1) s.remove(s.length() - 1);
  d.drawString(s.c_str(), W - 12, 16);
}

void drawScreen(bool cancellable) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "wifi setup");

  // Two QR codes, white quiet zone on the dark UI. ECC low keeps the join
  // code at version 3 (29 modules → 4 px each at 116 px).
  constexpr int QW = 116, QY = 50, QX1 = 22, QX2 = W - 22 - QW;
  String join = "WIFI:T:WPA;S:" + g_apSsid + ";P:" + g_apPass + ";;";
  d.fillRect(QX1 - 4, QY - 4, QW + 8, QW + 8, TFT_WHITE);
  d.qrcode(join.c_str(), QX1, QY, QW, 1);
  d.fillRect(QX2 - 4, QY - 4, QW + 8, QW + 8, TFT_WHITE);
  d.qrcode("http://192.168.4.1/", QX2, QY, QW, 1);

  d.setFont(&F_SMALL);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("1 . join", QX1 + QW / 2, QY + QW + 8);
  d.drawString("2 . open", QX2 + QW / 2, QY + QW + 8);
  d.setTextColor(COL_DIM, COL_BG);
  String creds = g_apSsid + "  key " + g_apPass + "  192.168.4.1";
  d.drawString(creds.c_str(), W / 2, QY + QW + 28);
#if WH_HAS_TOUCH
  // Touch has no physical B: a tappable cancel (button boards: B cancels).
  if (cancellable) {
    d.setFont(&F_SMALL);
    d.setTextDatum(middle_center);
    d.setTextColor(COL_DIM, COL_BG);
    d.drawString("cancel", W / 2, kCancelY);
  }
#else
  (void)cancellable;
#endif
  d.endWrite();
  drawStatus("waiting for phone", COL_DIM);
}

// ------------------------------------------------------------------- web ---

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// The web player's dark skin, system monospace (no webfonts offline).
const char kHead[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Waverz setup</title><style>"
    "body{background:#0a0a0a;color:#e8e8e8;font:16px/1.45 ui-monospace,Menlo,Consolas,"
    "monospace;margin:0 auto;padding:18px;max-width:460px}"
    "h1{color:#fff205;font-size:24px;margin:0 0 4px}.d{color:#777}a{color:#fff205}"
    "label.n{display:flex;justify-content:space-between;align-items:center;padding:11px 12px;"
    "margin:6px 0;background:#161616;border:1px solid #2a2a2a;border-radius:6px}"
    "label.n:has(input:checked){border-color:#fff205}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:11px;"
    "background:#161616;color:#e8e8e8;border:1px solid #2a2a2a;border-radius:6px;font:inherit}"
    "button{width:100%;padding:13px;margin-top:16px;background:#fff205;color:#0a0a0a;border:0;"
    "border-radius:6px;font:inherit;font-weight:bold}"
    "</style></head><body><h1>Waverz&middot;net</h1>";

String bars(int rssi) {
  int b = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -72 ? 2 : rssi >= -80 ? 1 : 0;
  String s;
  for (int i = 0; i < 4; ++i) s += i < b ? "&#9646;" : "&#9647;";
  return s;
}

void handleRoot() {
  String p;
  p.reserve(1800 + g_nets.size() * 160);
  p += FPSTR(kHead);
  p += "<p class=d>Pick the Wi-Fi this radio should join.</p><form method=post action=/join>";
  for (size_t i = 0; i < g_nets.size(); ++i) {
    String e = htmlEscape(g_nets[i].ssid);
    p += "<label class=n><span><input type=radio name=ssid value=\"" + e + "\"" +
         (i == 0 ? " checked" : "") + "> " + e + "</span><span class=d>" +
         (g_nets[i].secure ? "&#128274; " : "") + bars(g_nets[i].rssi) + "</span></label>";
  }
  if (g_nets.empty()) p += "<p class=d>No networks found &mdash; type the name below.</p>";
  p += "<label class=n><span><input type=radio name=ssid value='' id=o";
  p += g_nets.empty() ? " checked" : "";
  p += "> other network</span></label>"
       "<input type=text name=other maxlength=32 placeholder='network name' "
       "autocapitalize=none autocorrect=off onfocus='o.checked=true'>"
       "<p>password</p><input type=password name=pass id=p maxlength=63 autocapitalize=none "
       "autocorrect=off><label class=d><input type=checkbox "
       "onclick=\"p.type=this.checked?'text':'password'\"> show</label>"
       "<button>join</button></form>"
       "<p class=d><a href=/scan>rescan</a> &middot; 2.4 GHz networks only</p></body></html>";
  g_server->send(200, "text/html", p);
}

void sendMessage(int code, const String& html) {
  String p = FPSTR(kHead);
  p += html;
  p += "</body></html>";
  g_server->send(code, "text/html", p);
}

void handleJoin() {
  String ssid = g_server->arg("ssid");
  if (ssid.isEmpty()) ssid = g_server->arg("other");
  ssid.trim();
  String pass = g_server->arg("pass");
  if (ssid.isEmpty() || ssid.length() > 32) {
    return sendMessage(400, "<p>Pick a network (or type its name).</p><p><a href=/>back</a></p>");
  }
  if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
    return sendMessage(400, "<p>Wi-Fi passwords are 8&ndash;63 characters "
                            "(empty for an open network).</p><p><a href=/>back</a></p>");
  }
  if (g_state == JoinState::Joining || g_state == JoinState::Pending) {
    return sendMessage(409, "<p>Already joining &mdash; wait a moment.</p>");
  }
  g_joinSsid = ssid;
  g_joinPass = pass;
  g_state = JoinState::Pending;
  // The phone may lose this access point for a moment while the radio joins
  // (the AP follows the new network's channel) — the screen is the fallback.
  sendMessage(200,
              "<p id=m>Joining <b>" + htmlEscape(ssid) +
                  "</b> &hellip;</p><p class=d>Watch the radio's screen &mdash; this page may "
                  "lose its connection while the radio switches networks.</p><script>"
                  "function t(){fetch('/status').then(r=>r.json()).then(j=>{"
                  "if(j.s=='ok')m.innerHTML='Connected &#10003; &mdash; the radio is restarting. "
                  "You can close this page.';"
                  "else if(j.s=='fail')m.innerHTML='Could not join &mdash; check the password. "
                  "<a href=/>Try again</a>';else setTimeout(t,1000)}).catch(()=>setTimeout(t,1500))}"
                  "setTimeout(t,1500)</script>");
}

void handleStatus() {
  const char* s = g_state == JoinState::Ok     ? "ok"
                  : g_state == JoinState::Fail ? "fail"
                  : g_state == JoinState::Idle ? "idle"
                                               : "joining";
  g_server->send(200, "application/json", String("{\"s\":\"") + s + "\"}");
}

void scan() {
  int n = WiFi.scanNetworks();  // blocking ~2-4 s; results sorted strongest-first
  g_nets.clear();
  for (int i = 0; i < n && g_nets.size() < kMaxNets; ++i) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;  // hidden — reachable via "other network"
    bool dup = false;
    for (auto& e : g_nets) dup |= e.ssid == s;
    if (!dup) g_nets.push_back({s, (int)WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  log_i("portal: scan found %u networks", (unsigned)g_nets.size());
}

void handleScan() {
  drawStatus("scanning ...", COL_DIM);
  scan();  // the AP hops channels meanwhile — the phone may blink off briefly
  drawStatus("phone connected", COL_FG);
  g_server->sendHeader("Location", "/");
  g_server->send(302, "text/plain", "");
}

// Captive-portal catch-all: every unknown URL (the OS connectivity probes
// included — generate_204, hotspot-detect.html, connecttest.txt …) is
// redirected to the form, which makes phones pop it up by themselves.
void handleNotFound() {
  g_server->sendHeader("Location", "http://192.168.4.1/");
  g_server->send(302, "text/plain", "");
}

// ------------------------------------------------------------- sessions ---

void makeApCredentials() {
  String mac = WiFi.macAddress();  // AA:BB:CC:DD:EE:FF
  g_apSsid = "Waverz-" + mac.substring(12, 14) + mac.substring(15, 17);
  static const char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no 0/o/1/l/i
  g_apPass = "";
  for (int i = 0; i < 8; ++i) g_apPass += kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
}

// Verify the requested network while keeping the AP + page alive. A
// failure leaves the portal open for another try.
void doJoin() {
  g_state = JoinState::Joining;
  String msg = "joining " + g_joinSsid + " ...";
  drawStatus(msg.c_str(), COL_FG);
  log_i("portal: joining %s", g_joinSsid.c_str());

  WiFi.disconnect();
  bool accepted = false;
  for (int i = 0; i < 20 && !accepted; ++i) {  // driver may still be settling
    delay(100);
    accepted = WiFi.begin(g_joinSsid.c_str(), g_joinPass.c_str()) != WL_CONNECT_FAILED;
  }
  uint32_t deadline = millis() + kJoinTimeoutMs;
  while (accepted && WiFi.status() != WL_CONNECTED && millis() < deadline) {
    g_server->handleClient();
    delay(20);
  }
  if (accepted && WiFi.status() == WL_CONNECTED) {
    g_state = JoinState::Ok;
    whnvs::saveWifi(g_joinSsid, g_joinPass);
    log_i("portal: joined %s — saved, rebooting", g_joinSsid.c_str());
    drawStatus("connected - restarting", COL_OK);
    // Let the page's status poll see "ok" before the AP disappears.
    uint32_t until = millis() + 3000;
    while (millis() < until) {
      g_server->handleClient();
      delay(20);
    }
    ESP.restart();
  }
  g_state = JoinState::Fail;
  WiFi.disconnect();
  log_w("portal: join %s failed", g_joinSsid.c_str());
  msg = "failed: " + g_joinSsid;
  drawStatus(msg.c_str(), COL_ERR);
}

bool cancelPressed() {
  if (M5.BtnB.wasPressed()) return true;
#if WH_HAS_TOUCH
  auto t = M5.Touch.getDetail();
  if (t.wasClicked() && t.y >= kCancelY - 22 && t.y < H && t.x > W / 2 - 60 &&
      t.x < W / 2 + 60)
    return true;
#endif
  return false;
}

}  // namespace

namespace wifi_portal {

Result run(bool cancellable, uint32_t idleTimeoutMs) {
  auto& d = M5.Display;
  d.fillScreen(COL_BG);
  drawHeader(d, "wifi setup");
  d.setFont(&F_MED);
  d.setTextDatum(middle_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("scanning networks ...", W / 2, H / 2);

  // Quiet the STA (it may be cycling retries on an unreachable network),
  // scan from plain STA mode, then bring the AP up next to it.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(100);
  scan();
  makeApCredentials();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(g_apSsid.c_str(), g_apPass.c_str())) log_e("portal: softAP failed");
  log_i("portal: AP %s up at %s", g_apSsid.c_str(), WiFi.softAPIP().toString().c_str());

  g_dns = new DNSServer();
  g_dns->start(53, "*", kApIp);
  g_server = new WebServer(80);
  g_server->on("/", HTTP_GET, handleRoot);
  g_server->on("/join", HTTP_POST, handleJoin);
  g_server->on("/status", HTTP_GET, handleStatus);
  g_server->on("/scan", HTTP_GET, handleScan);
  g_server->onNotFound(handleNotFound);
  g_server->begin();
  g_state = JoinState::Idle;

  drawScreen(cancellable);

  Result result = Result::Cancelled;
  uint32_t idleSince = millis();
  uint8_t lastStations = 0;
  for (;;) {
    M5.update();
    serial_cmd::poll();  // bench: wifi-join over serial still works here
    g_server->handleClient();
    uint8_t stations = WiFi.softAPgetStationNum();
    if (stations) idleSince = millis();
    if (stations != lastStations && g_state != JoinState::Joining) {
      lastStations = stations;
      if (g_state != JoinState::Fail)
        drawStatus(stations ? "phone connected" : "waiting for phone", stations ? COL_FG : COL_DIM);
    }
    if (g_state == JoinState::Pending) doJoin();  // reboots on success
    if (cancellable && cancelPressed()) break;
    if (idleTimeoutMs && millis() - idleSince > idleTimeoutMs) {
      result = Result::TimedOut;
      break;
    }
    delay(2);
  }

  g_server->stop();
  delete g_server;
  g_server = nullptr;
  g_dns->stop();
  delete g_dns;
  g_dns = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  log_i("portal: closed (%s)", result == Result::TimedOut ? "idle timeout" : "cancelled");
  return result;
}

}  // namespace wifi_portal
