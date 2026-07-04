#include "ui.h"

#include <LittleFS.h>
#include <M5Unified.h>

#include "catalog.h"
#include "config.h"

namespace {

constexpr int W = 320, H = 240;
constexpr int MARQUEE_X = 8, MARQUEE_Y = 158, MARQUEE_W = W - 16, MARQUEE_H = 26;

M5Canvas g_card(&M5.Display);     // full card, rebuilt on change, pushed whole
M5Canvas g_marquee(&M5.Display);  // title strip, pushed only while scrolling

PlayerSnapshot g_snap;
NowPlaying g_np;
bool g_haveCard = false;

uint32_t g_overlayUntil = 0;  // volume bar or toast owns the screen until then

// Marquee state
String g_marqueeText;
int g_marqueeOffset = 0;
int g_marqueeTextW = 0;
bool g_marqueeActive = false;
uint32_t g_marqueeNextStep = 0;

String effectiveTitle() {
  if (!g_np.title.isEmpty()) return g_np.title;
  if (g_snap.streamTitle[0]) return String(g_snap.streamTitle);
  return "~ on air ~";
}

void setMarquee(const String& text) {
  g_marquee.setFont(&fonts::DejaVu18);
  g_marqueeTextW = g_marquee.textWidth(text.c_str());
  g_marqueeText = text;
  g_marqueeOffset = 0;
  g_marqueeActive = g_marqueeTextW > MARQUEE_W;
}

void drawMarqueeFrame() {
  g_marquee.fillScreen(TFT_BLACK);
  g_marquee.setFont(&fonts::DejaVu18);
  g_marquee.setTextColor(TFT_WHITE, TFT_BLACK);
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
  constexpr int SIZE = 128, X = 8, Y = 22;
  bool drawn = false;
  if (!s.iconPath.isEmpty() && LittleFS.exists(s.iconPath)) {
    // 64 px pack icons at 2x — nearest-neighbor fits the pixel aesthetic.
    drawn = g_card.drawPngFile(LittleFS, s.iconPath.c_str(), X, Y, SIZE, SIZE,
                               0, 0, 2.0f, 2.0f);
  }
  if (!drawn) {
    g_card.fillRoundRect(X, Y, SIZE, SIZE, 12, s.color565);
    g_card.setFont(&fonts::Font8x8C64);
    g_card.setTextSize(6);
    g_card.setTextDatum(middle_center);
    g_card.setTextColor(TFT_BLACK, s.color565);
    char initial[2] = {s.name.length() ? s.name[0] : '?', 0};
    g_card.drawString(initial, X + SIZE / 2, Y + SIZE / 2 + 4);
  }
}

void buildCard() {
  g_card.fillScreen(TFT_BLACK);

  if (g_snap.stationIndex < 0 || g_snap.stationIndex >= (int)catalog::count()) {
    g_card.setFont(&fonts::Font8x8C64);
    g_card.setTextSize(2);
    g_card.setTextDatum(middle_center);
    g_card.setTextColor(TFT_WHITE, TFT_BLACK);
    g_card.drawString(player::stateName(g_snap.state), W / 2, H / 2);
    return;
  }

  const Station& s = catalog::at(g_snap.stationIndex);
  g_card.fillRect(0, 0, W, 6, s.color565);
  drawIconOrPlaceholder(s);

  // Text column right of the icon.
  constexpr int TX = 148;
  g_card.setTextDatum(top_left);
  g_card.setFont(&fonts::Font8x8C64);
  g_card.setTextSize(2);
  g_card.setTextColor(TFT_WHITE, TFT_BLACK);
  // Wrap the station name onto up to 2 lines of ~10 chars.
  String name = s.name;
  if (g_card.textWidth(name.c_str()) <= W - TX - 8) {
    g_card.drawString(name.c_str(), TX, 34);
  } else {
    int cut = name.lastIndexOf(' ', 10);
    if (cut <= 0) cut = 10;
    g_card.drawString(name.substring(0, cut).c_str(), TX, 24);
    g_card.drawString(name.substring(cut + (name[cut] == ' ' ? 1 : 0)).c_str(), TX, 48);
  }
  g_card.setTextSize(1);
  g_card.setTextColor(s.color565, TFT_BLACK);
  String sub = s.channel.isEmpty() ? s.city : "ch " + s.channel + " . " + s.city;
  g_card.drawString(sub.c_str(), TX, 84);

  // Status / subtitle zone under the marquee strip.
  g_card.setFont(&fonts::DejaVu12);
  g_card.setTextDatum(top_center);
  g_card.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (g_snap.state == PlayerState::Playing) {
    if (!g_np.subtitle.isEmpty()) g_card.drawString(g_np.subtitle.c_str(), W / 2, 196);
  } else {
    String msg = player::stateName(g_snap.state);
    if (g_snap.state == PlayerState::Tuning) msg = "tuning ...";
    g_card.drawString(msg.c_str(), W / 2, 196);
  }

  // Nav hints.
  g_card.setFont(&fonts::Font8x8C64);
  g_card.setTextSize(1);
  g_card.setTextColor(0x39E7 /* dim grey */, TFT_BLACK);
  g_card.setTextDatum(bottom_left);
  g_card.drawString("<", 6, H - 4);
  g_card.setTextDatum(bottom_right);
  g_card.drawString(">", W - 6, H - 4);
}

void pushCard() {
  g_card.pushSprite(0, 0);
  if (g_snap.state == PlayerState::Playing) drawMarqueeFrame();
}

void drawVolumeBar(uint8_t vol) {
  auto& d = M5.Display;
  int barW = W - 80, x = 40, y = H - 46, h = 16;
  d.startWrite();
  d.fillRoundRect(x - 6, y - 24, barW + 12, h + 34, 8, TFT_BLACK);
  d.drawRoundRect(x - 6, y - 24, barW + 12, h + 34, 8, TFT_DARKGREY);
  d.fillRect(x, y, barW * vol / 21, h, TFT_WHITE);
  d.drawRect(x, y, barW, h, TFT_DARKGREY);
  d.setFont(&fonts::Font8x8C64);
  d.setTextSize(1);
  d.setTextDatum(top_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[16];
  snprintf(buf, sizeof(buf), "vol %u/21", vol);
  d.drawString(buf, W / 2, y - 18);
  d.endWrite();
}

void drawToast(int current) {
  auto& d = M5.Display;
  constexpr int TW = 260, TH = 150, TX = (W - TW) / 2, TY = (H - TH) / 2;
  int n = (int)catalog::count();
  d.startWrite();
  d.fillRoundRect(TX, TY, TW, TH, 10, TFT_BLACK);
  d.drawRoundRect(TX, TY, TW, TH, 10, TFT_DARKGREY);
  d.setFont(&fonts::Font8x8C64);
  d.setTextSize(1);
  for (int row = 0; row < 5; ++row) {
    int idx = current + row - 2;
    int y = TY + 12 + row * 27;
    if (idx < 0 || idx >= n) continue;  // no wrap in the display — shows list edges
    const Station& s = catalog::at(idx);
    String label = s.name + (s.channel.isEmpty() ? "" : " " + s.channel);
    if (row == 2) {
      d.fillRoundRect(TX + 8, y - 4, TW - 16, 24, 4, s.color565);
      d.setTextColor(TFT_BLACK, s.color565);
      d.setTextSize(2);
      d.setTextDatum(middle_left);
      d.drawString(label.c_str(), TX + 16, y + 8);
      d.setTextSize(1);
    } else {
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      d.setTextDatum(middle_left);
      d.drawString(label.c_str(), TX + 24, y + 8);
    }
  }
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

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::Font8x8C64);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
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
  setMarquee(effectiveTitle());
  buildCard();
  if (millis() >= g_overlayUntil) pushCard();
}

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
