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
constexpr int MARQUEE_X = 8, MARQUEE_Y = 150, MARQUEE_W = W - 16, MARQUEE_H = 36;

// Web player's default (Dark) skin palette — style.css :root.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG = rgb565(0x0a, 0x0a, 0x0a);    // --bg
constexpr uint16_t COL_FG = rgb565(0xe8, 0xe8, 0xe8);    // --fg
constexpr uint16_t COL_DIM = rgb565(0x66, 0x66, 0x66);   // muted labels/hints
constexpr uint16_t COL_LINE = rgb565(0x2a, 0x2a, 0x2a);  // borders/dividers
// accent + accent-fg come from the station (accent-fg = COL_BG, like the web).

const lgfx::GFXfont& F_SMALL = whfonts::VT323_16;
const lgfx::GFXfont& F_MED = whfonts::VT323_24;
const lgfx::GFXfont& F_BIG = whfonts::VT323_32;

M5Canvas g_card(&M5.Display);     // full card, rebuilt on change, pushed whole
M5Canvas g_marquee(&M5.Display);  // title strip, pushed only while scrolling

PlayerSnapshot g_snap;
NowPlaying g_np;
bool g_haveCard = false;

uint32_t g_overlayUntil = 0;  // volume bar or toast owns the screen until then

// Settings overlay state
enum class SettingsPage : uint8_t { Main, Stations, Wifi };
bool g_settingsOpen = false;
SettingsPage g_page = SettingsPage::Main;
AudioOutSetting g_setAout = AudioOutSetting::Auto;
AudioOutSetting g_setAoutOriginal = AudioOutSetting::Auto;
uint8_t g_setBright = 200;
std::vector<StationMeta> g_metas;  // stations page working copy
int g_metaScroll = 0;
bool g_stationsChanged = false;

const char* aoutName(AudioOutSetting a) {
  switch (a) {
    case AudioOutSetting::Internal:    return "internal";
    case AudioOutSetting::Rca:         return "rca module";
    case AudioOutSetting::ModuleAudio: return "module audio";
    default:                           return "auto";
  }
}

// Marquee state
String g_marqueeText;
int g_marqueeOffset = 0;
int g_marqueeTextW = 0;
bool g_marqueeActive = false;
uint32_t g_marqueeNextStep = 0;

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

String effectiveTitle() {
  if (!g_np.title.isEmpty()) return sanitizeForFont(g_np.title);
  // ICY fallback — some stations send a bare " - " separator as the title.
  String icy = sanitizeForFont(String(g_snap.streamTitle));
  String stripped(icy);
  stripped.replace("-", "");
  stripped.trim();
  if (!stripped.isEmpty()) return icy;
  return "~ on air ~";
}

// The marquee strip is rendered ONCE per title into a wide sprite; each frame
// only pushes a clipped scrolling window. Re-rendering text at 30 fps starved
// the stream's TLS pump enough to drain the audio cushion (found the hard way
// — NTS burned 98 KB of buffer in 11 s with a long title scrolling).
constexpr int MARQUEE_GAP = 60;

void setMarquee(const String& text) {
  g_card.setFont(&F_BIG);
  g_card.setTextSize(1);
  g_marqueeTextW = g_card.textWidth(text.c_str());
  g_marqueeText = text;
  g_marqueeOffset = 0;
  g_marqueeActive = g_marqueeTextW > MARQUEE_W;

  int stripW = g_marqueeActive ? g_marqueeTextW + MARQUEE_GAP + MARQUEE_W : MARQUEE_W;
  g_marquee.deleteSprite();
  g_marquee.createSprite(stripW, MARQUEE_H);
  g_marquee.fillScreen(COL_BG);
  g_marquee.setFont(&F_BIG);
  g_marquee.setTextSize(1);
  g_marquee.setTextColor(COL_FG, COL_BG);
  if (!g_marqueeActive) {
    g_marquee.setTextDatum(top_center);
    g_marquee.drawString(g_marqueeText.c_str(), MARQUEE_W / 2, 2);
  } else {
    g_marquee.setTextDatum(top_left);
    g_marquee.drawString(g_marqueeText.c_str(), 0, 2);
    g_marquee.drawString(g_marqueeText.c_str(), g_marqueeTextW + MARQUEE_GAP, 2);
  }
}

void drawMarqueeFrame() {
  if (!g_marqueeActive) {
    g_marquee.pushSprite(MARQUEE_X, MARQUEE_Y);
    return;
  }
  M5.Display.setClipRect(MARQUEE_X, MARQUEE_Y, MARQUEE_W, MARQUEE_H);
  g_marquee.pushSprite(MARQUEE_X - g_marqueeOffset, MARQUEE_Y);
  M5.Display.clearClipRect();
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

  // Status / subtitle zone under the marquee strip.
  g_card.setFont(&F_MED);
  g_card.setTextDatum(top_center);
  g_card.setTextColor(COL_DIM, COL_BG);
  if (g_snap.state == PlayerState::Playing) {
    if (!g_np.subtitle.isEmpty())
      g_card.drawString(sanitizeForFont(g_np.subtitle).c_str(), W / 2, 192);
  } else {
    String msg = player::stateName(g_snap.state);
    if (g_snap.state == PlayerState::Tuning) msg = "tuning ...";
    g_card.drawString(msg.c_str(), W / 2, 192);
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
  if (g_snap.state == PlayerState::Playing) drawMarqueeFrame();
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

bool settingsNeedsReboot() {
  return g_setAout != g_setAoutOriginal || g_stationsChanged;
}

void drawBottomButton(const char* label) {
  auto& d = M5.Display;
  d.setTextDatum(middle_center);
  d.setFont(&F_MED);
  d.fillRoundRect(W / 2 - 90, 198, 180, 32, 8, COL_LINE);
  d.setTextColor(COL_FG, COL_LINE);
  d.drawString(label, W / 2, 214);
}

void drawSettingsMain() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  d.setFont(&F_BIG);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("SETTINGS", W / 2, 6);

  d.setFont(&F_SMALL);
  d.setTextDatum(middle_left);
  d.setTextColor(COL_DIM, COL_BG);
  d.drawString("audio out", 16, 58);
  d.drawString("brightness", 16, 90);
  d.setTextDatum(middle_right);
  d.setFont(&F_MED);
  d.setTextColor(COL_FG, COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "< %s >", aoutName(g_setAout));
  d.drawString(buf, W - 16, 58);
  snprintf(buf, sizeof(buf), "< %d%% >", g_setBright * 100 / 255);
  d.drawString(buf, W - 16, 90);

  d.setTextDatum(middle_left);
  d.drawString("stations ...", 16, 126);
  d.drawString("wifi ...", 16, 158);

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_DIM, COL_BG);
  snprintf(buf, sizeof(buf), "fw %s (%d)", WH_FW_VERSION, WH_FW_BUILD);
  String info = String(buf) + "  c:" + catalog::contentVersion().substring(0, 8);
  d.drawString(info.c_str(), 16, 178);

  drawBottomButton(settingsNeedsReboot() ? "SAVE+REBOOT" : "CLOSE");
  d.endWrite();
}

constexpr int kMetaRows = 6;

void drawSettingsStations() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  d.setFont(&F_MED);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("STATIONS", W / 2, 4);

  d.setFont(&F_SMALL);
  for (int row = 0; row < kMetaRows; ++row) {
    int i = g_metaScroll + row;
    if (i >= (int)g_metas.size()) break;
    int y = 34 + row * 27;
    d.drawRect(12, y, 16, 16, COL_DIM);
    if (g_metas[i].visible) d.fillRect(15, y + 3, 10, 10, rgb565(0x2a, 0xeb, 0x62));
    d.setTextDatum(middle_left);
    d.setTextColor(g_metas[i].visible ? COL_FG : COL_DIM, COL_BG);
    d.drawString(g_metas[i].label.c_str(), 40, y + 8);
  }
  d.setTextDatum(middle_center);
  d.setTextColor(COL_DIM, COL_BG);
  char pos[16];
  snprintf(pos, sizeof(pos), "%d-%d / %d", g_metaScroll + 1,
           (int)std::min<size_t>(g_metaScroll + kMetaRows, g_metas.size()),
           (int)g_metas.size());
  d.drawString(pos, W / 2, 34 + kMetaRows * 27 + 6);

  d.setFont(&F_MED);
  d.setTextColor(COL_FG, COL_LINE);
  d.fillRoundRect(8, 198, 70, 32, 8, COL_LINE);
  d.setTextDatum(middle_center);
  d.drawString("^", 8 + 35, 214);
  d.fillRoundRect(W - 78, 198, 70, 32, 8, COL_LINE);
  d.drawString("v", W - 78 + 35, 214);
  d.fillRoundRect(W / 2 - 55, 198, 110, 32, 8, COL_LINE);
  d.drawString("BACK", W / 2, 214);
  d.endWrite();
}

void drawSettingsWifi() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  d.setFont(&F_MED);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("WIFI", W / 2, 4);

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_FG, COL_BG);
  String l1 = "ssid: " + WiFi.SSID();
  String l2 = "ip:   " + WiFi.localIP().toString();
  String l3 = "rssi: " + String(WiFi.RSSI()) + " dBm";
  d.drawString(l1.c_str(), 16, 48);
  d.drawString(l2.c_str(), 16, 74);
  d.drawString(l3.c_str(), 16, 100);
  d.setTextColor(COL_DIM, COL_BG);
  d.drawString("scan + join with on-screen", 16, 140);
  d.drawString("keyboard: coming soon", 16, 160);

  drawBottomButton("BACK");
  d.endWrite();
}

void drawSettings() {
  switch (g_page) {
    case SettingsPage::Stations: drawSettingsStations(); break;
    case SettingsPage::Wifi:     drawSettingsWifi(); break;
    default:                     drawSettingsMain(); break;
  }
}

}  // namespace

namespace ui {

void begin(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
  M5.Display.setRotation(1);

  g_card.setPsram(true);
  g_card.setColorDepth(16);
  g_card.createSprite(W, H);
  g_marquee.setPsram(true);
  g_marquee.setColorDepth(16);
  g_marquee.createSprite(MARQUEE_W, MARQUEE_H);

  M5.Display.fillScreen(COL_BG);
  M5.Display.setFont(&F_SMALL);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COL_FG, COL_BG);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextScroll(true);
}

void bootLine(const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  M5.Display.println(buf);
}

void render(const PlayerSnapshot& snap, const NowPlaying& np) {
  g_snap = snap;
  g_np = np;
  g_haveCard = true;
  log_i("card: state=%s title='%s' subtitle='%s'", player::stateName(snap.state),
        effectiveTitle().c_str(), np.subtitle.c_str());
  setMarquee(effectiveTitle());
  buildCard();
  if (!g_settingsOpen && millis() >= g_overlayUntil) pushCard();
}

bool settingsOpen() { return g_settingsOpen; }

void settingsShow(AudioOutSetting audioOut, uint8_t brightness) {
  g_settingsOpen = true;
  g_page = SettingsPage::Main;
  g_setAout = g_setAoutOriginal = audioOut;
  g_setBright = brightness;
  g_stationsChanged = false;
  drawSettings();
}

SettingsAction settingsTouch(int x, int y) {
  if (g_page == SettingsPage::Stations) {
    if (y >= 34 && y < 34 + kMetaRows * 27) {  // toggle a row
      int i = g_metaScroll + (y - 34) / 27;
      if (i >= 0 && i < (int)g_metas.size()) {
        catalog::toggleUserVisible(g_metas[i].id);
        g_metas[i].visible = !g_metas[i].visible;
        g_stationsChanged = true;
        drawSettings();
      }
    } else if (y > 190) {
      if (x < 100) {  // page up
        g_metaScroll = std::max(0, g_metaScroll - kMetaRows);
        drawSettings();
      } else if (x > W - 100) {  // page down
        if (g_metaScroll + kMetaRows < (int)g_metas.size()) g_metaScroll += kMetaRows;
        drawSettings();
      } else {  // BACK
        g_page = SettingsPage::Main;
        drawSettings();
      }
    }
    return SettingsAction::None;
  }

  if (g_page == SettingsPage::Wifi) {
    if (y > 190) {
      g_page = SettingsPage::Main;
      drawSettings();
    }
    return SettingsAction::None;
  }

  // Main page
  if (y > 190 && x > W / 2 - 90 && x < W / 2 + 90) {  // CLOSE / SAVE+REBOOT
    g_settingsOpen = false;
    bool reboot = settingsNeedsReboot();
    if (!reboot && g_haveCard) pushCard();
    return reboot ? SettingsAction::CloseAndReboot : SettingsAction::Close;
  }
  if (y > 42 && y < 74) {  // audio out cycle
    g_setAout = static_cast<AudioOutSetting>((static_cast<uint8_t>(g_setAout) + 1) % 4);
    drawSettings();
  } else if (y > 74 && y < 108) {  // brightness cycle, applied live
    static const uint8_t levels[] = {60, 120, 200, 255};
    size_t i = 0;
    while (i < 3 && levels[i] <= g_setBright) ++i;
    g_setBright = levels[g_setBright >= 255 ? 0 : i];
    M5.Display.setBrightness(g_setBright);
    drawSettings();
  } else if (y > 108 && y < 142) {  // stations page
    g_metas = catalog::allMeta();
    g_metaScroll = 0;
    g_page = SettingsPage::Stations;
    drawSettings();
  } else if (y > 142 && y < 172) {  // wifi page
    g_page = SettingsPage::Wifi;
    drawSettings();
  }
  return SettingsAction::None;
}

AudioOutSetting settingsAudioOut() { return g_setAout; }
uint8_t settingsBrightness() { return g_setBright; }

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
  if (g_overlayUntil) return;  // overlays freeze the marquee

  if (g_haveCard && g_marqueeActive && g_snap.state == PlayerState::Playing &&
      now >= g_marqueeNextStep) {
    g_marqueeNextStep = now + 50;  // 20 fps is plenty for text
    g_marqueeOffset += 3;
    if (g_marqueeOffset >= g_marqueeTextW + MARQUEE_GAP) g_marqueeOffset = 0;
    drawMarqueeFrame();
  }
}

}  // namespace ui
