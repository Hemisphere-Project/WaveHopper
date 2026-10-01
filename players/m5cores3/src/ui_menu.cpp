// Button-driven settings menu — boards without touch (the Fire).
// A list per page: A = up, C = down, B tap = select, B hold = back/close
// (no on-screen button hints — the selection highlight carries it).
// The touch boards' settings overlay lives in ui.cpp; both speak the same
// ui::SettingsAction to main.
#include "ui.h"

#include "config.h"

#if !WH_HAS_TOUCH

#include <WiFi.h>

#include <vector>

#include "catalog.h"
#include "ui_internal.h"

namespace {

using namespace ui::detail;

enum class Page : uint8_t { Main, Wifi, Stations, About };

Page g_page = Page::Main;
int g_sel = 0;     // selected row on the current page
int g_scroll = 0;  // first visible row (long lists)
uint8_t g_bright = 200;
bool g_stationsChanged = false;
bool g_forgetArmed = false;  // "forget network" needs a second select
std::vector<StationMeta> g_metas;

struct Row {
  String label;
  String value;     // right-aligned (accent); empty = none
  int check = -1;   // -1 none, 0 unchecked, 1 checked (station visibility)
};

constexpr uint8_t kBrightLevels[] = {60, 120, 200, 255};

std::vector<Row> rows() {
  std::vector<Row> r;
  switch (g_page) {
    case Page::Main: {
      bool up = WiFi.status() == WL_CONNECTED;
      r.push_back({"wifi", up ? WiFi.SSID() : String("offline")});
      r.push_back({"stations", ">"});
      r.push_back({"brightness", String(g_bright * 100 / 255) + "%"});
      r.push_back({"about", ">"});
      r.push_back({g_stationsChanged ? "save + reboot" : "close", ""});
      break;
    }
    case Page::Wifi:
      r.push_back({"phone setup", ">"});
      r.push_back({g_forgetArmed ? "forget? select again" : "forget network", ""});
      r.push_back({"back", ""});
      break;
    case Page::Stations:
      r.push_back({"back", ""});
      for (auto& m : g_metas) r.push_back({m.label, "", m.visible ? 1 : 0});
      break;
    case Page::About:
      r.push_back({"reboot", ""});
      r.push_back({"back", ""});
      break;
  }
  return r;
}

// Info lines above the list (wifi + about pages). Returns the y where the
// list starts.
int drawInfo(LovyanGFX& d, int y) {
  auto line = [&](const String& s) {
    d.drawString(s.c_str(), 16, y);
    y += 20;
  };
  d.setFont(&F_SMALL);
  d.setTextDatum(top_left);
  d.setTextColor(COL_DIM, COL_BG);
  bool up = WiFi.status() == WL_CONNECTED;
  if (g_page == Page::Wifi) {
    line("ssid  " + (up ? WiFi.SSID() : String("(offline)")));
    line("ip    " + (up ? WiFi.localIP().toString() : String("-")) +
         (up ? "   " + String(WiFi.RSSI()) + " dBm" : String()));
  } else if (g_page == Page::About) {
    char buf[64];
    snprintf(buf, sizeof(buf), "fw %s (build %d)  %s", WH_FW_VERSION, WH_FW_BUILD,
             WH_BOARD_NAME);
    line(buf);
    line("content " + catalog::contentVersion().substring(0, 12));
    line("ip " + (up ? WiFi.localIP().toString() : String("-")) + "  " + WiFi.macAddress());
    snprintf(buf, sizeof(buf), "heap %lu/%lu  psram %lu KB", (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getMaxAllocHeap(), (unsigned long)(ESP.getFreePsram() / 1024));
    line(buf);
  }
  return y + 4;
}

void draw() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(COL_BG);
  static const char* const kTitles[] = {"settings", "wifi", "stations", "about"};
  drawHeader(d, kTitles[(int)g_page]);

  int y0 = drawInfo(d, 52);
  std::vector<Row> r = rows();
  const bool compact = g_page == Page::Stations;
  const int rowH = compact ? 30 : 36;
  const int visible = std::max(1, (H - 4 - y0) / rowH);
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + visible) g_scroll = g_sel - visible + 1;

  for (int i = 0; i < visible && g_scroll + i < (int)r.size(); ++i) {
    const Row& row = r[g_scroll + i];
    const bool sel = g_scroll + i == g_sel;
    const int y = y0 + i * rowH, h = rowH - 4;
    d.fillRoundRect(12, y, W - 24, h, 6, COL_PANEL);
    if (sel) d.drawRoundRect(12, y, W - 24, h, 6, COL_ACCENT);
    int tx = 24;
    if (row.check >= 0) {
      uint16_t c = row.check ? COL_OK : COL_LINE;
      d.drawRoundRect(22, y + (h - 16) / 2, 16, 16, 3, c);
      if (row.check) d.fillRoundRect(25, y + (h - 16) / 2 + 3, 10, 10, 2, c);
      tx = 46;
    }
    d.setFont(&F_MED);
    d.setTextDatum(middle_left);
    d.setTextColor(sel ? COL_ACCENT : (row.check == 0 ? COL_DIM : COL_FG), COL_PANEL);
    String label = row.label;
    while (d.textWidth(label.c_str()) > W - tx - 40 && label.length() > 1)
      label.remove(label.length() - 1);
    d.drawString(label.c_str(), tx, y + h / 2 + 1);
    if (row.value.length()) {
      d.setTextDatum(middle_right);
      d.setTextColor(COL_ACCENT, COL_PANEL);
      String v = row.value;
      if (v.length() > 16) v = v.substring(0, 15) + "~";
      d.drawString(v.c_str(), W - 24, y + h / 2 + 1);
    }
  }
  // Scroll position for long lists.
  if ((int)r.size() > visible) {
    d.setFont(&F_SMALL);
    d.setTextDatum(top_right);
    d.setTextColor(COL_DIM, COL_BG);
    char pos[16];
    snprintf(pos, sizeof(pos), "%d/%d", g_sel + 1, (int)r.size());
    d.drawString(pos, W - 12, 34 - 14);
  }
  d.endWrite();
}

void go(Page p) {
  g_page = p;
  g_sel = 0;
  g_scroll = 0;
  g_forgetArmed = false;
  draw();
}

ui::SettingsAction close() {
  g_settingsOpen = false;
  if (g_stationsChanged) return ui::SettingsAction::CloseAndReboot;
  restoreCard();
  return ui::SettingsAction::Close;
}

ui::SettingsAction select() {
  switch (g_page) {
    case Page::Main:
      switch (g_sel) {
        case 0: go(Page::Wifi); break;
        case 1:
          g_metas = catalog::allMeta();
          go(Page::Stations);
          break;
        case 2: {  // brightness — cycle, applied live
          size_t i = 0;
          while (i < sizeof(kBrightLevels) - 1 && kBrightLevels[i] <= g_bright) ++i;
          g_bright = g_bright >= 255 ? kBrightLevels[0] : kBrightLevels[i];
          M5.Display.setBrightness(g_bright);
          draw();
          break;
        }
        case 3: go(Page::About); break;
        default: return close();
      }
      return ui::SettingsAction::None;

    case Page::Wifi:
      if (g_sel == 0) {
        g_settingsOpen = false;
        return ui::SettingsAction::PhoneSetup;
      }
      if (g_sel == 1) {
        if (!g_forgetArmed) {
          g_forgetArmed = true;
          draw();
          return ui::SettingsAction::None;
        }
        g_settingsOpen = false;
        return ui::SettingsAction::ForgetWifi;
      }
      go(Page::Main);
      return ui::SettingsAction::None;

    case Page::Stations:
      if (g_sel == 0) {
        go(Page::Main);
      } else if (g_sel - 1 < (int)g_metas.size()) {
        StationMeta& m = g_metas[g_sel - 1];
        catalog::toggleUserVisible(m.id);
        m.visible = !m.visible;
        g_stationsChanged = true;
        draw();
      }
      return ui::SettingsAction::None;

    case Page::About:
      if (g_sel == 0) {
        g_settingsOpen = false;
        return ui::SettingsAction::CloseAndReboot;
      }
      go(Page::Main);
      return ui::SettingsAction::None;
  }
  return ui::SettingsAction::None;
}

}  // namespace

namespace ui {

void settingsShow(AudioOutSetting audioOut, uint8_t brightness) {
  (void)audioOut;  // auto-detected; no manual selection
  g_settingsOpen = true;
  g_bright = brightness;
  g_stationsChanged = false;
  go(Page::Main);
}

SettingsAction settingsKey(MenuKey k) {
  if (!g_settingsOpen) return SettingsAction::None;
  const int n = (int)rows().size();
  switch (k) {
    case MenuKey::Up:
      g_sel = (g_sel - 1 + n) % n;
      g_forgetArmed = false;
      draw();
      return SettingsAction::None;
    case MenuKey::Down:
      g_sel = (g_sel + 1) % n;
      g_forgetArmed = false;
      draw();
      return SettingsAction::None;
    case MenuKey::Select:
      return select();
    case MenuKey::Back:
      if (g_page == Page::Main) return close();
      go(Page::Main);
      return SettingsAction::None;
  }
  return SettingsAction::None;
}

uint8_t settingsBrightness() { return g_bright; }

// Touch-only entry points — never reached on a button board.
SettingsAction settingsTouch(int, int) { return SettingsAction::None; }
bool settingsScroll(int) { return false; }
String settingsWifiSsid() { return String(); }
String settingsWifiPassword() { return String(); }
void settingsWifiResult(bool) {}

}  // namespace ui

#endif  // !WH_HAS_TOUCH
