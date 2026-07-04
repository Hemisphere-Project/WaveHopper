#include "ui.h"

#include <M5Unified.h>

#include "catalog.h"
#include "config.h"

namespace {

PlayerSnapshot g_last;
bool g_haveCard = false;
uint32_t g_overlayUntil = 0;

constexpr int W = 320, H = 240;

void drawCard(const PlayerSnapshot& snap) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);

  if (snap.stationIndex >= 0 && snap.stationIndex < (int)catalog::count()) {
    const Station& s = catalog::at(snap.stationIndex);

    d.fillRect(0, 0, W, 6, s.color565);  // accent bar

    d.setTextDatum(top_center);
    d.setFont(&fonts::Font8x8C64);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(2);
    d.drawString(s.name.c_str(), W / 2, 36);

    d.setTextSize(1);
    d.setTextColor(s.color565, TFT_BLACK);
    String sub = s.channel.isEmpty() ? s.city : s.channel + " . " + s.city;
    d.drawString(sub.c_str(), W / 2, 78);

    // State / now-playing area
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    if (snap.state == PlayerState::Playing) {
      d.setFont(&fonts::DejaVu18);
      String title = snap.streamTitle;
      if (title.isEmpty()) title = "~ on air ~";
      // Naive two-line wrap; marquee lands in M2.
      d.setTextDatum(top_center);
      if (d.textWidth(title.c_str()) <= W - 20) {
        d.drawString(title.c_str(), W / 2, 130);
      } else {
        int cut = title.length() / 2;
        while (cut < (int)title.length() && title[cut] != ' ') cut++;
        d.drawString(title.substring(0, cut).c_str(), W / 2, 118);
        d.drawString(title.substring(cut).c_str(), W / 2, 146);
      }
    } else {
      d.setFont(&fonts::Font8x8C64);
      d.setTextSize(1);
      String msg = player::stateName(snap.state);
      if (snap.state == PlayerState::Tuning) msg = "tuning...";
      d.drawString(msg.c_str(), W / 2, 132);
    }

    // Hints
    d.setFont(&fonts::Font8x8C64);
    d.setTextSize(1);
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setTextDatum(bottom_left);
    d.drawString("< prev", 8, H - 8);
    d.setTextDatum(bottom_right);
    d.drawString("next >", W - 8, H - 8);
  } else {
    d.setTextDatum(middle_center);
    d.setFont(&fonts::Font8x8C64);
    d.setTextSize(2);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(player::stateName(snap.state), W / 2, H / 2);
  }
  d.endWrite();
}

void drawVolume(uint8_t vol) {
  auto& d = M5.Display;
  int barW = W - 80;
  int x = 40, y = H - 46, h = 16;
  d.startWrite();
  d.fillRoundRect(x - 6, y - 6, barW + 12, h + 12, 6, TFT_BLACK);
  d.drawRoundRect(x - 6, y - 6, barW + 12, h + 12, 6, TFT_DARKGREY);
  d.fillRect(x, y, barW * vol / 21, h, TFT_WHITE);
  d.setFont(&fonts::Font8x8C64);
  d.setTextSize(1);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[16];
  snprintf(buf, sizeof(buf), "vol %u", vol);
  d.drawString(buf, W / 2, y - 16);
  d.endWrite();
}

}  // namespace

namespace ui {

void begin(uint8_t brightness) {
  M5.Display.setBrightness(brightness);
  M5.Display.setRotation(1);
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

void render(const PlayerSnapshot& snap) {
  g_last = snap;
  g_haveCard = true;
  if (millis() < g_overlayUntil) return;  // overlay owns the screen right now
  drawCard(snap);
}

void volumeOverlay(uint8_t vol) {
  if (!g_haveCard) return;
  if (millis() >= g_overlayUntil) drawCard(g_last);  // fresh card under the bar
  g_overlayUntil = millis() + 1500;
  drawVolume(vol);
}

void tick() {
  if (g_overlayUntil && millis() >= g_overlayUntil) {
    g_overlayUntil = 0;
    if (g_haveCard) drawCard(g_last);
  }
}

}  // namespace ui
