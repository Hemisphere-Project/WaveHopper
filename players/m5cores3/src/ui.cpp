#include "ui.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>

#include <algorithm>

#include "catalog.h"
#include "config.h"
#include "font_vt323.h"

namespace {

constexpr int W = 320, H = 240;
constexpr int MARQUEE_X = 8, MARQUEE_W = W - 16, MARQUEE_H = 30;
constexpr int MARQUEE_Y1 = 150, MARQUEE_Y2 = 184;  // title line, subtitle line
constexpr int MARQUEE_GAP = 60;

// Web player's default (Dark) skin palette — style.css :root.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG = rgb565(0x0a, 0x0a, 0x0a);      // --bg
constexpr uint16_t COL_FG = rgb565(0xe8, 0xe8, 0xe8);      // --fg
constexpr uint16_t COL_DIM = rgb565(0x66, 0x66, 0x66);     // muted labels/hints
constexpr uint16_t COL_LINE = rgb565(0x2a, 0x2a, 0x2a);    // borders/dividers
constexpr uint16_t COL_ACCENT = rgb565(0xff, 0xf2, 0x05);  // --accent (yellow) for chrome
constexpr uint16_t COL_PANEL = rgb565(0x16, 0x16, 0x16);   // row/panel fill
// per-station accent (accent-fg = COL_BG, like the web) comes from color565.

const lgfx::GFXfont& F_SMALL = whfonts::VT323_20;
const lgfx::GFXfont& F_MED = whfonts::VT323_24;   // metadata (title + subtitle)
const lgfx::GFXfont& F_BIG = whfonts::VT323_34;   // station name

// Page header: accent title + underline. Returns the y below it for content.
int drawHeader(LovyanGFX& d, const char* title) {
  d.setFont(&F_BIG);
  d.setTextSize(1);
  d.setTextDatum(top_left);
  d.setTextColor(COL_ACCENT, COL_BG);
  d.drawString(title, 12, 6);
  d.drawFastHLine(12, 44, W - 24, COL_LINE);
  return 54;
}

M5Canvas g_card(&M5.Display);  // full card, rebuilt on change, pushed whole

// A single-line strip that scrolls when its text overflows. Rendered once per
// text change into a wide sprite; each frame pushes a clipped window (cheap —
// re-rendering text at frame rate starved the stream's TLS pump).
struct Marquee {
  M5Canvas sprite{&M5.Display};
  String text;
  int y = 0, textW = 0, offset = 0;
  bool active = false;
  uint32_t nextStep = 0;

  void begin(int y_) {
    y = y_;
    sprite.setPsram(true);
    sprite.setColorDepth(16);
    sprite.createSprite(MARQUEE_W, MARQUEE_H);
  }
  void set(const String& t, uint16_t fg) {
    g_card.setFont(&F_MED);
    g_card.setTextSize(1);
    textW = g_card.textWidth(t.c_str());
    text = t;
    offset = 0;
    active = textW > MARQUEE_W;
    int stripW = active ? textW + MARQUEE_GAP + MARQUEE_W : MARQUEE_W;
    sprite.deleteSprite();
    sprite.createSprite(stripW, MARQUEE_H);
    sprite.fillScreen(COL_BG);
    sprite.setFont(&F_MED);
    sprite.setTextSize(1);
    sprite.setTextColor(fg, COL_BG);
    int cy = MARQUEE_H / 2;
    if (!active) {
      sprite.setTextDatum(middle_center);
      sprite.drawString(text.c_str(), MARQUEE_W / 2, cy);
    } else {
      sprite.setTextDatum(middle_left);
      sprite.drawString(text.c_str(), 0, cy);
      sprite.drawString(text.c_str(), textW + MARQUEE_GAP, cy);
    }
  }
  void frame() {
    if (!active) {
      sprite.pushSprite(MARQUEE_X, y);
      return;
    }
    M5.Display.setClipRect(MARQUEE_X, y, MARQUEE_W, MARQUEE_H);
    sprite.pushSprite(MARQUEE_X - offset, y);
    M5.Display.clearClipRect();
  }
  void tick(uint32_t now) {
    if (!active || now < nextStep) return;
    nextStep = now + 50;  // 20 fps, 3 px/frame
    offset += 3;
    if (offset >= textW + MARQUEE_GAP) offset = 0;
    frame();
  }
};

Marquee g_title, g_sub;  // white now-playing title + grey subtitle, both scroll

PlayerSnapshot g_snap;
NowPlaying g_np;
bool g_haveCard = false;

uint32_t g_overlayUntil = 0;  // volume bar or toast owns the screen until then

// Settings overlay state
enum class SettingsPage : uint8_t { Main, Stations, Wifi, WifiScan, WifiPassword };
bool g_settingsOpen = false;
SettingsPage g_page = SettingsPage::Main;
uint8_t g_setBright = 200;
std::vector<StationMeta> g_metas;  // stations page working copy
int g_metaScroll = 0;
bool g_stationsChanged = false;

// Wi-Fi scan + on-screen keyboard state.
std::vector<String> g_ssids;
int g_ssidScroll = 0;
String g_selSSID;
String g_pw;
uint8_t g_kbLayer = 0;  // 0 lower, 1 upper, 2 symbols

// Keyboard: 3 character rows × 10 keys, per layer, + a control row.
const char* const kKbRows[3][3] = {
    {"qwertyuiop", "asdfghjkl.", "zxcvbnm-_@"},  // lower
    {"QWERTYUIOP", "ASDFGHJKL.", "ZXCVBNM-_@"},  // upper
    {"1234567890", "!#$%&*()+=", ":;,?/[]{}~"},  // symbols
};
constexpr int kKbCols = 10, kKbKeyW = 32, kKbKeyH = 30, kKbY0 = 116;

// Metadata: resolved two-line block (white title marquee + grey subtitle).
String g_metaSub;

// The embedded VT323 covers Latin-1 only. Keep valid ≤U+00FF sequences, map
// common typographic/status codepoints to ASCII, drop the rest — wavezero's
// live-dot (●/🔴) and friends would otherwise render as nothing/garbage.
String sanitizeForFont(const String& in) {
  String out;
  out.reserve(in.length());
  const uint8_t* p = (const uint8_t*)in.c_str();
  while (*p) {
    uint32_t cp = 0;
    int len = 1;
    if (*p < 0x80) {
      cp = *p;
    } else if ((*p >> 5) == 0x6 && (p[1] & 0xC0) == 0x80) {
      cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      len = 2;
    } else if ((*p >> 4) == 0xE && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      len = 3;
    } else if ((*p >> 3) == 0x1E && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
               (p[3] & 0xC0) == 0x80) {
      cp = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) |
           (p[3] & 0x3F);
      len = 4;
    }
    if (cp <= 0xFF) {
      for (int i = 0; i < len; ++i) out += (char)p[i];  // keep original bytes
    } else {
      switch (cp) {
        case 0x2018: case 0x2019: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2010: case 0x2013: case 0x2014: out += '-'; break;
        case 0x2026: out += "..."; break;
        case 0x2022: case 0x25CF: case 0x26AB: case 0x2B24: case 0x25C9:
        case 0x1F534: case 0x1F518: out += '*'; break;
        default: break;  // drop silently
      }
    }
    p += len;
  }
  return out;
}

// RSSI bars, drawable into the card canvas or straight onto the display.
void drawMeterInto(LovyanGFX& g) {
  int rssi = WiFi.RSSI();
  int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -72 ? 2 : rssi >= -80 ? 1 : 0;
  uint16_t col = bars >= 3   ? rgb565(0x14, 0x66, 0x2e)
                 : bars == 2 ? rgb565(0x66, 0x4d, 0x12)
                             : rgb565(0x70, 0x16, 0x16);
  int x0 = W - 30, y0 = 10;
  g.fillRect(x0 - 2, y0 - 1, 26, 14, COL_BG);
  for (int b = 0; b < 4; ++b) {
    int h = 3 + b * 3;
    g.fillRect(x0 + b * 6, y0 + 12 - h, 4, h, b < bars ? col : COL_LINE);
  }
}

// Resolve the metadata into a title (white marquee) + subtitle (grey line),
// setting g_metaSub. Splits an "Artist - Track" ICY string across the two.
String resolveMeta() {
  String title = sanitizeForFont(g_np.title);
  g_metaSub = sanitizeForFont(g_np.subtitle);
  if (title.isEmpty()) {
    // ICY fallback — often "Artist - Track"; a bare " - " means nothing.
    String icy = sanitizeForFont(String(g_snap.streamTitle));
    String stripped(icy);
    stripped.replace("-", "");
    stripped.trim();
    if (stripped.isEmpty()) { g_metaSub = ""; return "~ on air ~"; }
    title = icy;
  }
  if (g_metaSub.isEmpty()) {
    int d = title.indexOf(" - ");
    if (d > 0 && d < (int)title.length() - 3) {
      g_metaSub = title.substring(0, d);      // artist → grey line
      title = title.substring(d + 3);         // track → white line
    }
  }
  return title;
}

void drawIconOrPlaceholder(const Station& s) {
  constexpr int SIZE = 128, X = 8, Y = 16;
  bool drawn = false;
  if (!s.iconPath.isEmpty() && LittleFS.exists(s.iconPath)) {
    // 64 px pack icons at 2x — nearest-neighbor fits the pixel aesthetic.
    drawn = g_card.drawPngFile(LittleFS, s.iconPath.c_str(), X, Y, SIZE, SIZE,
                               0, 0, 2.0f, 2.0f);
  }
  if (!drawn) {
    g_card.fillRoundRect(X, Y, SIZE, SIZE, 12, s.color565);
    g_card.setFont(&F_BIG);
    g_card.setTextSize(2);
    g_card.setTextDatum(middle_center);
    g_card.setTextColor(COL_BG, s.color565);
    char initial[2] = {s.name.length() ? s.name[0] : '?', 0};
    g_card.drawString(initial, X + SIZE / 2, Y + SIZE / 2);
    g_card.setTextSize(1);
  }
}

void buildCard() {
  g_card.fillScreen(COL_BG);

  if (g_snap.stationIndex < 0 || g_snap.stationIndex >= (int)catalog::count()) {
    g_card.setFont(&F_MED);
    g_card.setTextDatum(middle_center);
    g_card.setTextColor(COL_FG, COL_BG);
    g_card.drawString(player::stateName(g_snap.state), W / 2, H / 2);
    return;
  }

  const Station& s = catalog::at(g_snap.stationIndex);
  g_card.fillRect(0, 0, W, 6, s.color565);
  drawIconOrPlaceholder(s);

  // Text column right of the icon.
  constexpr int TX = 148;
  g_card.setTextDatum(top_left);
  g_card.setFont(&F_BIG);
  g_card.setTextSize(1);
  g_card.setTextColor(COL_FG, COL_BG);
  String name = s.name;
  int subY;
  if (g_card.textWidth(name.c_str()) <= W - TX - 8) {
    g_card.drawString(name.c_str(), TX, 34);
    subY = 66;  // city rides right under the name, subtitle-style
  } else {
    int cut = name.lastIndexOf(' ', 10);
    if (cut <= 0) cut = 10;
    g_card.drawString(name.substring(0, cut).c_str(), TX, 22);
    g_card.drawString(name.substring(cut + (name[cut] == ' ' ? 1 : 0)).c_str(), TX, 56);
    subY = 90;
  }
  g_card.setFont(&F_SMALL);
  g_card.setTextColor(s.color565, COL_BG);
  // Channel shown only when it distinguishes ("main" and empty don't).
  String sub = (s.channel.isEmpty() || s.channel == "main")
                   ? s.city
                   : s.channel + " . " + s.city;
  g_card.drawString(sub.c_str(), TX, subY);

  drawMeterInto(g_card);  // RSSI bars live in the card — never blink away

  // Metadata area (both lines) is owned by the two scrolling marquees, pushed
  // over the card in pushCard(). When not playing, show the state here instead.
  if (g_snap.state != PlayerState::Playing) {
    g_card.setFont(&F_MED);
    g_card.setTextDatum(middle_center);
    g_card.setTextColor(COL_DIM, COL_BG);
    String msg = player::stateName(g_snap.state);
    if (g_snap.state == PlayerState::Tuning) msg = "tuning ...";
    g_card.drawString(msg.c_str(), W / 2, MARQUEE_Y1 + MARQUEE_H / 2);
  }

  // Nav hints.
  g_card.setFont(&F_SMALL);
  g_card.setTextColor(COL_DIM, COL_BG);
  g_card.setTextDatum(bottom_left);
  g_card.drawString("<", 6, H - 6);  // clear of the bottom buffer gauge
  g_card.setTextDatum(bottom_right);
  g_card.drawString(">", W - 6, H - 6);
}

void pushCard() {
  g_card.pushSprite(0, 0);
  if (g_snap.state == PlayerState::Playing) {
    g_title.frame();
    g_sub.frame();
  }
}

void drawVolumeBar(uint8_t vol) {
  auto& d = M5.Display;
  int barW = W - 80, x = 40, y = H - 46, h = 16;
  d.startWrite();
  d.fillRoundRect(x - 6, y - 26, barW + 12, h + 36, 8, COL_BG);
  d.drawRoundRect(x - 6, y - 26, barW + 12, h + 36, 8, COL_LINE);
  d.fillRect(x, y, barW * vol / 21, h, COL_FG);
  d.drawRect(x, y, barW, h, COL_DIM);
  d.setFont(&F_SMALL);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  char buf[16];
  snprintf(buf, sizeof(buf), "vol %u/21", vol);
  d.drawString(buf, W / 2, y - 22);
  d.endWrite();
}

void drawToast(int current) {
  auto& d = M5.Display;
  constexpr int TW = 260, TH = 150, TX = (W - TW) / 2, TY = (H - TH) / 2;
  int n = (int)catalog::count();
  d.startWrite();
  d.fillRoundRect(TX, TY, TW, TH, 10, COL_BG);
  d.drawRoundRect(TX, TY, TW, TH, 10, COL_LINE);
  for (int row = 0; row < 5; ++row) {
    int idx = current + row - 2;
    int y = TY + 12 + row * 27;
    if (idx < 0 || idx >= n) continue;  // no wrap — shows list edges
    const Station& s = catalog::at(idx);
    bool showCh = !s.channel.isEmpty() && s.channel != "main";
    String label = s.name + (showCh ? " " + s.channel : "");
    if (row == 2) {
      // Active row: accent background, dark text — the web's active-row look.
      d.fillRoundRect(TX + 8, y - 4, TW - 16, 26, 4, s.color565);
      d.setFont(&F_MED);
      d.setTextColor(COL_BG, s.color565);
      d.setTextDatum(middle_left);
      d.drawString(label.c_str(), TX + 16, y + 9);
    } else {
      d.setFont(&F_SMALL);
      d.setTextColor(COL_FG, COL_BG);
      d.setTextDatum(middle_left);
      d.drawString(label.c_str(), TX + 24, y + 9);
    }
  }
  d.endWrite();
}

void drawBottomButton(const char* label) {
  auto& d = M5.Display;
  d.setTextDatum(middle_center);
  d.setFont(&F_MED);
  d.fillRoundRect(W / 2 - 90, 198, 180, 34, 8, COL_PANEL);
  d.drawRoundRect(W / 2 - 90, 198, 180, 34, 8, COL_LINE);
  d.setTextColor(COL_FG, COL_PANEL);
  d.drawString(label, W / 2, 215);
}

// A tappable full-width row with a label and an optional right-aligned value.
void drawRow(LovyanGFX& d, int y, const char* label, const char* value) {
  d.fillRoundRect(12, y, W - 24, 34, 6, COL_PANEL);
  d.setFont(&F_MED);
  d.setTextColor(COL_FG, COL_PANEL);
  d.setTextDatum(middle_left);
  d.drawString(label, 24, y + 18);
  if (value) {
    d.setTextColor(COL_ACCENT, COL_PANEL);
    d.setTextDatum(middle_right);
    d.drawString(value, W - 24, y + 18);
  }
}

void drawSettingsMain() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "settings");

  char buf[24];
  snprintf(buf, sizeof(buf), "%d%%", g_setBright * 100 / 255);
  drawRow(d, 58, "brightness", buf);
  drawRow(d, 98, "stations", ">");
  drawRow(d, 138, "wifi", ">");

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_DIM, COL_BG);
  snprintf(buf, sizeof(buf), "fw %s (%d)", WH_FW_VERSION, WH_FW_BUILD);
  String info = String(buf) + "  c:" + catalog::contentVersion().substring(0, 8);
  d.drawString(info.c_str(), 14, 178);

  drawBottomButton(g_stationsChanged ? "SAVE + REBOOT" : "CLOSE");
  d.endWrite();
}

constexpr int kMetaRows = 5, kMetaRowH = 28, kMetaY0 = 56;

void drawSettingsStations() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "stations");

  d.setFont(&F_MED);
  for (int row = 0; row < kMetaRows; ++row) {
    int i = g_metaScroll + row;
    if (i >= (int)g_metas.size()) break;
    int y = kMetaY0 + row * kMetaRowH;
    bool on = g_metas[i].visible;
    d.fillRoundRect(12, y, W - 24, kMetaRowH - 4, 5, COL_PANEL);
    d.drawRoundRect(20, y + 5, 16, 16, 3, on ? rgb565(0x2a, 0xeb, 0x62) : COL_LINE);
    if (on) d.fillRoundRect(23, y + 8, 10, 10, 2, rgb565(0x2a, 0xeb, 0x62));
    d.setTextDatum(middle_left);
    d.setTextColor(on ? COL_FG : COL_DIM, COL_PANEL);
    d.drawString(g_metas[i].label.c_str(), 46, y + (kMetaRowH - 4) / 2);
  }
  d.setFont(&F_SMALL);
  d.setTextDatum(middle_right);
  d.setTextColor(COL_DIM, COL_BG);
  char pos[24];
  snprintf(pos, sizeof(pos), "swipe . %d/%d",
           (int)std::min<size_t>(g_metaScroll + kMetaRows, g_metas.size()),
           (int)g_metas.size());
  d.drawString(pos, W - 14, 44 - 10);

  drawBottomButton("BACK");
  d.endWrite();
}

// Wi-Fi page 1: current connection + actions.
void drawSettingsWifi() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "wifi");

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  bool up = WiFi.status() == WL_CONNECTED;
  d.setTextColor(up ? COL_FG : COL_DIM, COL_BG);
  d.drawString(("ssid: " + (up ? WiFi.SSID() : String("(offline)"))).c_str(), 16, 60);
  d.drawString(("ip:   " + WiFi.localIP().toString()).c_str(), 16, 82);
  d.drawString(("rssi: " + String(WiFi.RSSI()) + " dBm").c_str(), 16, 104);

  d.setFont(&F_MED);
  d.setTextDatum(middle_center);
  d.fillRoundRect(W / 2 - 100, 128, 200, 42, 8, COL_PANEL);
  d.drawRoundRect(W / 2 - 100, 128, 200, 42, 8, COL_ACCENT);
  d.setTextColor(COL_ACCENT, COL_PANEL);
  d.drawString("scan networks", W / 2, 149);

  drawBottomButton("BACK");
  d.endWrite();
}

// Wi-Fi page 2: scrollable scan results.
void drawSettingsWifiScan() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "networks");

  if (g_ssids.empty()) {
    d.setFont(&F_MED);
    d.setTextDatum(middle_center);
    d.setTextColor(COL_DIM, COL_BG);
    d.drawString("no networks found", W / 2, 120);
  }
  d.setFont(&F_MED);
  for (int row = 0; row < kMetaRows; ++row) {
    int i = g_ssidScroll + row;
    if (i >= (int)g_ssids.size()) break;
    int y = kMetaY0 + row * kMetaRowH;
    d.fillRoundRect(12, y, W - 24, kMetaRowH - 4, 5, COL_PANEL);
    d.setTextDatum(middle_left);
    d.setTextColor(COL_FG, COL_PANEL);
    String s = g_ssids[i];
    while (d.textWidth(s.c_str()) > W - 56 && s.length() > 1) s.remove(s.length() - 1);
    d.drawString(s.c_str(), 24, y + (kMetaRowH - 4) / 2);
  }
  if (!g_ssids.empty()) {
    d.setFont(&F_SMALL);
    d.setTextDatum(middle_right);
    d.setTextColor(COL_DIM, COL_BG);
    char pos[24];
    snprintf(pos, sizeof(pos), "swipe . %d/%d",
             (int)std::min<size_t>(g_ssidScroll + kMetaRows, g_ssids.size()),
             (int)g_ssids.size());
    d.drawString(pos, W - 14, 34);
  }
  drawBottomButton("BACK");
  d.endWrite();
}

// Wi-Fi page 3: password field + on-screen keyboard.
void drawKeyboard() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_DIM, COL_BG);
  String ssid = g_selSSID;
  if (ssid.length() > 26) ssid = ssid.substring(0, 25) + "~";
  d.drawString(("join: " + ssid).c_str(), 8, 6);
  // Password field (shown plain — long WPA keys are error-prone to type blind).
  d.drawRect(8, 26, W - 16, 26, COL_LINE);
  d.setTextColor(COL_FG, COL_BG);
  d.setFont(&F_MED);
  d.setTextDatum(middle_left);
  String shown = g_pw.length() > 30 ? "~" + g_pw.substring(g_pw.length() - 29) : g_pw;
  d.drawString((shown + "_").c_str(), 14, 40);

  // Character grid.
  d.setFont(&F_MED);
  d.setTextDatum(middle_center);
  int startX = (W - kKbCols * kKbKeyW) / 2;
  for (int r = 0; r < 3; ++r) {
    const char* keys = kKbRows[g_kbLayer][r];
    int n = strlen(keys);
    int rowStartX = (W - n * kKbKeyW) / 2;
    for (int c = 0; c < n; ++c) {
      int x = rowStartX + c * kKbKeyW, y = kKbY0 + r * kKbKeyH;
      d.drawRoundRect(x + 1, y + 1, kKbKeyW - 2, kKbKeyH - 2, 4, COL_LINE);
      d.setTextColor(COL_FG, COL_BG);
      char lbl[2] = {keys[c], 0};
      d.drawString(lbl, x + kKbKeyW / 2, y + kKbKeyH / 2);
    }
  }
  (void)startX;
  // Control row: [shift/123] [space] [del] [OK].
  int cy = kKbY0 + 3 * kKbKeyH + 2, ch = 34;
  auto ctrl = [&](int x, int w, const char* lbl, uint16_t bg) {
    d.fillRoundRect(x, cy, w, ch, 6, bg);
    d.setTextColor(COL_FG, bg);
    d.drawString(lbl, x + w / 2, cy + ch / 2);
  };
  ctrl(6, 60, g_kbLayer == 2 ? "abc" : (g_kbLayer == 1 ? "sym" : "shft"), COL_LINE);
  ctrl(72, 120, "space", COL_LINE);
  ctrl(198, 52, "del", COL_LINE);
  ctrl(256, 58, "OK", rgb565(0x14, 0x66, 0x2e));
  d.endWrite();
}

// Full-screen "joining" notice: the join attempt blocks the UI task for up
// to ~12 s — without this the keyboard looks frozen after OK.
void drawWifiConnecting() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  drawHeader(d, "wifi");
  d.setFont(&F_MED);
  d.setTextDatum(middle_center);
  d.setTextColor(COL_FG, COL_BG);
  String ssid = g_selSSID;
  if (ssid.length() > 20) ssid = ssid.substring(0, 19) + "~";
  d.drawString(("joining " + ssid + " ...").c_str(), W / 2, 110);
  d.setFont(&F_SMALL);
  d.setTextColor(COL_DIM, COL_BG);
  d.drawString("takes a few seconds", W / 2, 140);
  d.endWrite();
}

void drawSettings() {
  switch (g_page) {
    case SettingsPage::Stations:     drawSettingsStations(); break;
    case SettingsPage::Wifi:         drawSettingsWifi(); break;
    case SettingsPage::WifiScan:     drawSettingsWifiScan(); break;
    case SettingsPage::WifiPassword: drawKeyboard(); break;
    default:                         drawSettingsMain(); break;
  }
}

void doWifiScan() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  d.setFont(&F_MED);
  d.setTextDatum(middle_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("scanning ...", W / 2, H / 2);
  d.endWrite();

  // The radio rejects scan starts while an association attempt is in flight
  // (the offline-boot case: the stack cycles connect retries continuously).
  // Only when offline: drop the attempt for a clean scan — settingsPump
  // re-kicks the stored network when settings closes or a join fails.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    delay(100);
  }
  int n = WiFi.scanNetworks();  // blocks ~2-4 s; briefly perturbs playback
  g_ssids.clear();
  for (int i = 0; i < n; ++i) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (auto& e : g_ssids) if (e == s) { dup = true; break; }
    if (!dup) g_ssids.push_back(s);  // strongest-first (scan is sorted by RSSI)
  }
  WiFi.scanDelete();
  g_ssidScroll = 0;
}

}  // namespace

namespace ui {

void begin(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
  M5.Display.setRotation(1);

  g_card.setPsram(true);
  g_card.setColorDepth(16);
  g_card.createSprite(W, H);
  g_title.begin(MARQUEE_Y1);
  g_sub.begin(MARQUEE_Y2);

  bootScreen();
}

// Settings gear in the boot header (top-right): tap target for the boot wifi
// wait — no glyph for it in the embedded font, so it's drawn (8 tooth nubs
// around a body circle, punched hub).
constexpr int kGearCX = W - 26, kGearCY = 30, kGearR = 9;

void bootScreen() {
  auto& d = M5.Display;
  d.fillScreen(COL_BG);
  // Fixed splash header; boot status scrolls in the region below it.
  d.setFont(&F_BIG);
  d.setTextDatum(top_center);
  d.setTextColor(COL_ACCENT, COL_BG);
  d.drawString("Waverz\xC2\xB7net", W / 2, 20);
  for (int i = 0; i < 8; ++i) {
    float a = i * (float)PI / 4;
    d.fillCircle(kGearCX + lroundf(cosf(a) * kGearR),
                 kGearCY + lroundf(sinf(a) * kGearR), 2, COL_DIM);
  }
  d.fillCircle(kGearCX, kGearCY, kGearR - 1, COL_DIM);
  d.fillCircle(kGearCX, kGearCY, 3, COL_BG);
  d.drawFastHLine(20, 82, W - 40, COL_LINE);

  d.setFont(&F_SMALL);
  d.setTextSize(1);
  d.setTextColor(COL_FG, COL_BG);
  d.setTextDatum(top_left);
  d.setCursor(14, 92);
  d.setTextScroll(true);
  d.setScrollRect(0, 90, W, H - 90);  // keep the header out of the scroll region
}

bool bootGearHit(int x, int y) {
  // Generous corner target around the drawn gear.
  return x >= kGearCX - 22 && y <= kGearCY + 22;
}

void bootLine(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  M5.Display.setTextDatum(top_left);
  M5.Display.println(buf);
}

void render(const PlayerSnapshot& snap, const NowPlaying& np) {
  g_snap = snap;
  g_np = np;
  g_haveCard = true;
  String title = resolveMeta();  // also sets g_metaSub
  log_i("card: state=%s title='%s' subtitle='%s'", player::stateName(snap.state),
        title.c_str(), g_metaSub.c_str());
  g_title.set(title, COL_FG);
  g_sub.set(g_metaSub, COL_DIM);
  buildCard();
  if (!g_settingsOpen && millis() >= g_overlayUntil) pushCard();
}

bool settingsOpen() { return g_settingsOpen; }

void settingsShow(AudioOutSetting audioOut, uint8_t brightness) {
  (void)audioOut;  // audio output is auto-detected now; no manual selection
  g_settingsOpen = true;
  g_page = SettingsPage::Main;
  g_setBright = brightness;
  g_stationsChanged = false;
  g_pw = "";
  drawSettings();
}

// Vertical drag scrolls the active list page (stations / scan). +dy = drag down
// = show earlier entries. Returns true if it consumed the gesture.
bool settingsScroll(int rows) {
  if (rows == 0) return false;
  if (g_page == SettingsPage::Stations) {
    int maxScroll = std::max(0, (int)g_metas.size() - kMetaRows);
    g_metaScroll = std::min(maxScroll, std::max(0, g_metaScroll - rows));
    drawSettings();
    return true;
  }
  if (g_page == SettingsPage::WifiScan) {
    int maxScroll = std::max(0, (int)g_ssids.size() - kMetaRows);
    g_ssidScroll = std::min(maxScroll, std::max(0, g_ssidScroll - rows));
    drawSettings();
    return true;
  }
  return false;
}

SettingsAction settingsTouch(int x, int y) {
  switch (g_page) {
    case SettingsPage::Stations:
      if (y >= kMetaY0 && y < kMetaY0 + kMetaRows * kMetaRowH) {
        int i = g_metaScroll + (y - kMetaY0) / kMetaRowH;
        if (i >= 0 && i < (int)g_metas.size()) {
          catalog::toggleUserVisible(g_metas[i].id);
          g_metas[i].visible = !g_metas[i].visible;
          g_stationsChanged = true;
          drawSettings();
        }
      } else if (y > 190) {
        g_page = SettingsPage::Main;
        drawSettings();
      }
      return SettingsAction::None;

    case SettingsPage::Wifi:
      if (y >= 128 && y <= 170 && x > W / 2 - 100 && x < W / 2 + 100) {
        doWifiScan();
        g_page = SettingsPage::WifiScan;
        drawSettings();
      } else if (y > 190) {
        g_page = SettingsPage::Main;
        drawSettings();
      }
      return SettingsAction::None;

    case SettingsPage::WifiScan:
      if (y >= kMetaY0 && y < kMetaY0 + kMetaRows * kMetaRowH) {
        int i = g_ssidScroll + (y - kMetaY0) / kMetaRowH;
        if (i >= 0 && i < (int)g_ssids.size()) {
          g_selSSID = g_ssids[i];
          g_pw = "";
          g_kbLayer = 0;
          g_page = SettingsPage::WifiPassword;
          drawSettings();
        }
      } else if (y > 190) {
        g_page = SettingsPage::Wifi;
        drawSettings();
      }
      return SettingsAction::None;

    case SettingsPage::WifiPassword: {
      // Character grid?
      if (y >= kKbY0 && y < kKbY0 + 3 * kKbKeyH) {
        int r = (y - kKbY0) / kKbKeyH;
        const char* keys = kKbRows[g_kbLayer][r];
        int n = strlen(keys);
        int rowStartX = (W - n * kKbKeyW) / 2;
        int c = (x - rowStartX) / kKbKeyW;
        if (c >= 0 && c < n && x >= rowStartX) {
          if (g_pw.length() < 63) g_pw += keys[c];
          drawSettings();
        }
        return SettingsAction::None;
      }
      // Control row.
      int cy = kKbY0 + 3 * kKbKeyH + 2;
      if (y >= cy && y < cy + 34) {
        if (x < 66) {  // shift / layer cycle: lower→upper→symbols→lower
          g_kbLayer = (g_kbLayer + 1) % 3;
        } else if (x < 192) {  // space
          if (g_pw.length() < 63) g_pw += ' ';
        } else if (x < 250) {  // del
          if (g_pw.length()) g_pw.remove(g_pw.length() - 1);
        } else {  // OK → hand credentials to main for verify+save
          drawWifiConnecting();
          return SettingsAction::ConnectWifi;
        }
        drawSettings();
        return SettingsAction::None;
      }
      // Tapping the field area cancels back to the scan list.
      if (y < 26) {
        g_page = SettingsPage::WifiScan;
        drawSettings();
      }
      return SettingsAction::None;
    }

    default:  // Main
      if (y > 190 && x > W / 2 - 90 && x < W / 2 + 90) {  // CLOSE / SAVE+REBOOT
        g_settingsOpen = false;
        if (!g_stationsChanged && g_haveCard) pushCard();
        return g_stationsChanged ? SettingsAction::CloseAndReboot : SettingsAction::Close;
      }
      if (y >= 58 && y < 92) {  // brightness row — cycle, applied live
        static const uint8_t levels[] = {60, 120, 200, 255};
        size_t i = 0;
        while (i < 3 && levels[i] <= g_setBright) ++i;
        g_setBright = levels[g_setBright >= 255 ? 0 : i];
        M5.Display.setBrightness(g_setBright);
        drawSettings();
      } else if (y >= 98 && y < 132) {  // stations row
        g_metas = catalog::allMeta();
        g_metaScroll = 0;
        g_page = SettingsPage::Stations;
        drawSettings();
      } else if (y >= 138 && y < 172) {  // wifi row
        g_page = SettingsPage::Wifi;
        drawSettings();
      }
      return SettingsAction::None;
  }
}

uint8_t settingsBrightness() { return g_setBright; }
String settingsWifiSsid() { return g_selSSID; }
String settingsWifiPassword() { return g_pw; }

void settingsWifiResult(bool ok) {
  auto& d = M5.Display;
  if (ok) return;  // success reboots; only failure returns here
  g_page = SettingsPage::WifiScan;
  drawSettings();
  d.setFont(&F_SMALL);
  d.setTextDatum(bottom_center);
  d.setTextColor(rgb565(0xeb, 0x2a, 0x2a), COL_BG);
  d.drawString("connect failed - pick again", W / 2, 194);
}

void stationToast(int currentIndex) {
  if (!g_haveCard) return;
  g_overlayUntil = millis() + 1200;
  drawToast(currentIndex);
}

void volumeOverlay(uint8_t vol) {
  if (!g_haveCard) return;
  if (millis() >= g_overlayUntil) pushCard();  // fresh card under the bar
  g_overlayUntil = millis() + 1500;
  drawVolumeBar(vol);
}

void bufferGauge(uint32_t buffered, uint32_t target) {
  if (g_settingsOpen || !g_haveCard || millis() < g_overlayUntil) return;
  if (!target) return;
  int w = std::min<uint32_t>(W, (uint64_t)buffered * W / target);
  // Whisper-quiet tones — a glance instrument, not a design element.
  uint16_t col = (buffered * 2 >= target)   ? rgb565(0x0a, 0x38, 0x1a)
                 : (buffered * 5 >= target) ? rgb565(0x38, 0x2c, 0x0a)
                                            : rgb565(0x40, 0x0c, 0x0c);
  M5.Display.fillRect(0, H - 2, w, 2, col);  // bottom edge — clear of the accent bar
  M5.Display.fillRect(w, H - 2, W - w, 2, COL_BG);
}

void wifiMeter(int rssi) {
  (void)rssi;  // meter reads WiFi.RSSI() itself; kept for API stability
  if (g_settingsOpen || !g_haveCard || millis() < g_overlayUntil) return;
  M5.Display.startWrite();
  drawMeterInto(M5.Display);
  M5.Display.endWrite();
}

void tick() {
  if (g_settingsOpen) return;  // modal — nothing else touches the screen
  uint32_t now = millis();
  if (g_overlayUntil && now >= g_overlayUntil) {
    g_overlayUntil = 0;
    if (g_haveCard) pushCard();
  }
  if (g_overlayUntil) return;  // overlays freeze the marquees

  if (g_haveCard && g_snap.state == PlayerState::Playing) {
    g_title.tick(now);
    g_sub.tick(now);
  }
}

}  // namespace ui
