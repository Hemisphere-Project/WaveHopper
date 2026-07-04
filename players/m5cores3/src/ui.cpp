#include "ui.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <WiFi.h>

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
bool g_settingsOpen = false;
AudioOutSetting g_setAout = AudioOutSetting::Auto;
AudioOutSetting g_setAoutOriginal = AudioOutSetting::Auto;
uint8_t g_setBright = 200;

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

String effectiveTitle() {
  if (!g_np.title.isEmpty()) return g_np.title;
  // ICY fallback — some stations send a bare " - " separator as the title.
  String icy(g_snap.streamTitle);
  String stripped(icy);
  stripped.replace("-", "");
  stripped.trim();
  if (!stripped.isEmpty()) return icy;
  return "~ on air ~";
}

void setMarquee(const String& text) {
  g_marquee.setFont(&F_BIG);
  g_marquee.setTextSize(1);
  g_marqueeTextW = g_marquee.textWidth(text.c_str());
  g_marqueeText = text;
  g_marqueeOffset = 0;
  g_marqueeActive = g_marqueeTextW > MARQUEE_W;
}

void drawMarqueeFrame() {
  g_marquee.fillScreen(COL_BG);
  g_marquee.setFont(&F_BIG);
  g_marquee.setTextSize(1);
  g_marquee.setTextColor(COL_FG, COL_BG);
  if (!g_marqueeActive) {
    g_marquee.setTextDatum(top_center);
    g_marquee.drawString(g_marqueeText.c_str(), MARQUEE_W / 2, 2);
  } else {
    constexpr int GAP = 60;
    g_marquee.setTextDatum(top_left);
    int x = -g_marqueeOffset;
    g_marquee.drawString(g_marqueeText.c_str(), x, 2);
    g_marquee.drawString(g_marqueeText.c_str(), x + g_marqueeTextW + GAP, 2);
  }
  g_marquee.pushSprite(MARQUEE_X, MARQUEE_Y);
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
  if (g_card.textWidth(name.c_str()) <= W - TX - 8) {
    g_card.drawString(name.c_str(), TX, 34);
  } else {
    int cut = name.lastIndexOf(' ', 10);
    if (cut <= 0) cut = 10;
    g_card.drawString(name.substring(0, cut).c_str(), TX, 22);
    g_card.drawString(name.substring(cut + (name[cut] == ' ' ? 1 : 0)).c_str(), TX, 56);
  }
  g_card.setFont(&F_SMALL);
  g_card.setTextColor(s.color565, COL_BG);
  // Channel shown only when it distinguishes ("main" and empty don't).
  String sub = (s.channel.isEmpty() || s.channel == "main")
                   ? s.city
                   : s.channel + " . " + s.city;
  g_card.drawString(sub.c_str(), TX, 96);

  // Status / subtitle zone under the marquee strip.
  g_card.setFont(&F_MED);
  g_card.setTextDatum(top_center);
  g_card.setTextColor(COL_DIM, COL_BG);
  if (g_snap.state == PlayerState::Playing) {
    if (!g_np.subtitle.isEmpty()) g_card.drawString(g_np.subtitle.c_str(), W / 2, 192);
  } else {
    String msg = player::stateName(g_snap.state);
    if (g_snap.state == PlayerState::Tuning) msg = "tuning ...";
    g_card.drawString(msg.c_str(), W / 2, 192);
  }

  // Nav hints.
  g_card.setFont(&F_SMALL);
  g_card.setTextColor(COL_DIM, COL_BG);
  g_card.setTextDatum(bottom_left);
  g_card.drawString("<", 6, H - 2);
  g_card.setTextDatum(bottom_right);
  g_card.drawString(">", W - 6, H - 2);
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
    String label = s.name + (s.channel.isEmpty() ? "" : " " + s.channel);
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

void drawSettings() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  d.setFont(&F_BIG);
  d.setTextDatum(top_center);
  d.setTextColor(COL_FG, COL_BG);
  d.drawString("SETTINGS", W / 2, 8);

  d.setFont(&F_SMALL);
  d.setTextDatum(middle_left);
  d.setTextColor(COL_DIM, COL_BG);
  d.drawString("audio out", 16, 62);
  d.drawString("brightness", 16, 100);
  d.setTextDatum(middle_right);
  d.setFont(&F_MED);
  d.setTextColor(COL_FG, COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "< %s >", aoutName(g_setAout));
  d.drawString(buf, W - 16, 62);
  snprintf(buf, sizeof(buf), "< %d%% >", g_setBright * 100 / 255);
  d.drawString(buf, W - 16, 100);

  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_DIM, COL_BG);
  snprintf(buf, sizeof(buf), "fw %s (%d)", WH_FW_VERSION, WH_FW_BUILD);
  d.drawString(buf, 16, 134);
  String cv = "content " + catalog::contentVersion().substring(0, 12);
  d.drawString(cv.c_str(), 16, 152);
  String ip = "ip " + WiFi.localIP().toString();
  d.drawString(ip.c_str(), 16, 170);

  d.setTextDatum(middle_center);
  d.setFont(&F_MED);
  bool reboots = g_setAout != g_setAoutOriginal;
  d.fillRoundRect(W / 2 - 90, 198, 180, 32, 8, COL_LINE);
  d.setTextColor(COL_FG, COL_LINE);
  d.drawString(reboots ? "SAVE+REBOOT" : "CLOSE", W / 2, 214);
  d.endWrite();
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
  g_setAout = g_setAoutOriginal = audioOut;
  g_setBright = brightness;
  drawSettings();
}

SettingsAction settingsTouch(int x, int y) {
  if (y > 190 && x > W / 2 - 90 && x < W / 2 + 90) {  // CLOSE / SAVE+REBOOT
    g_settingsOpen = false;
    bool reboot = g_setAout != g_setAoutOriginal;
    if (!reboot && g_haveCard) pushCard();
    return reboot ? SettingsAction::CloseAndReboot : SettingsAction::Close;
  }
  if (y > 46 && y < 80) {  // audio out cycle
    g_setAout = static_cast<AudioOutSetting>((static_cast<uint8_t>(g_setAout) + 1) % 4);
    drawSettings();
  } else if (y > 84 && y < 118) {  // brightness cycle, applied live
    static const uint8_t levels[] = {60, 120, 200, 255};
    size_t i = 0;
    while (i < 3 && levels[i] <= g_setBright) ++i;
    g_setBright = levels[g_setBright >= 255 ? 0 : i];
    M5.Display.setBrightness(g_setBright);
    drawSettings();
  }
  return SettingsAction::None;
}

AudioOutSetting settingsAudioOut() { return g_setAout; }
uint8_t settingsBrightness() { return g_setBright; }

void stationToast(int currentIndex) {
  if (!g_haveCard) return;
  g_overlayUntil = millis() + 2000;
  drawToast(currentIndex);
}

void volumeOverlay(uint8_t vol) {
  if (!g_haveCard) return;
  if (millis() >= g_overlayUntil) pushCard();  // fresh card under the bar
  g_overlayUntil = millis() + 1500;
  drawVolumeBar(vol);
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
    g_marqueeNextStep = now + 33;  // ~30 fps, 2 px/frame
    g_marqueeOffset += 2;
    if (g_marqueeOffset >= g_marqueeTextW + 60) g_marqueeOffset = 0;
    drawMarqueeFrame();
  }
}

}  // namespace ui
