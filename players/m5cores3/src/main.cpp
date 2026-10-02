// WaveHopper M5 firmware (CoreS3 touch / Fire buttons) — boot sequencer +
// input/UI loop.
//
// Boot: display → FS → NVS → wifi (retries forever, interactive; the phone
// setup portal opens by itself when there is no network to try) → catalog →
// audio profile → player task → auto-play. Content sync + firmware OTA slot in
// before catalog load. Playback policy: no play/pause — the supervisor keeps
// something playing (retry once → skip → sweep).
// Controls, touch (CoreS3): tap left/right = prev/next, vertical drag = browse,
//   bezel BtnA/BtnC = volume; hold the card / BtnB (or the boot gear) = settings.
// Controls, buttons (Fire): A/C tap = prev/next, A/C hold = browse (tunes when
//   released), B tap = volume mode (A/C = -/+), B hold = settings menu; hold B
//   at power-on = straight into the Wi-Fi setup portal.

#include <M5Unified.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>

#include "config.h"
#include "wh_nvs.h"
#include "wh_wifi.h"
#include "audio_out.h"
#include "catalog.h"
#include "content_sync.h"
#include "fw_update.h"
#include "net.h"
#include "now_playing.h"
#include "player.h"
#include "serial_cmd.h"
#include "telemetry.h"
#include "ui.h"
#include "wifi_portal.h"

static WhSettings settings;
static AudioProfile profile = AudioProfile::Internal;
static uint32_t lastRenderedGen = 0;
static uint32_t lastNpGen = 0;
static int lastStationIndex = -1;

static bool g_booting = true;           // until the player runs (setup() done)
static bool g_portalRequested = false;  // settings asked for the portal during boot
static int g_browseIdx = -1;            // >=0 while browsing the station toast
static bool g_wakeTouch = false;        // touch boards: this gesture woke the screen
static uint32_t g_browseCommitAt = 0;   // tune the browsed station after this

// Settings closed with an action — shared by the touch overlay and the
// button menu.
static void applySettingsAction(ui::SettingsAction action) {
  using A = ui::SettingsAction;
  if (action == A::None) return;
  settings.brightness = ui::settingsBrightness();
  whnvs::saveBrightness(settings.brightness);
  switch (action) {
    case A::CloseAndReboot:
      ESP.restart();
      return;
    case A::PhoneSetup:
      // The portal needs the radio + heap to itself: during the boot wifi
      // wait it runs right away, otherwise via a clean boot (before the player).
      if (g_booting) {
        g_portalRequested = true;
        return;
      }
      whnvs::setPortalOnBoot();
      ESP.restart();
      return;
    case A::ForgetWifi:
      whnvs::forgetWifi();  // next boot: no credentials → portal
      ESP.restart();
      return;
    default:
      break;
  }
  // The wifi scan drops an in-progress association to get a clean scan —
  // make sure the stored network is trying again once settings closes.
  if (!whwifi::isConnected()) whwifi::beginConnect(settings);
}

#if WH_HAS_TOUCH
// Modal settings pump: drag-scroll + tap routing. Shared by loop() and the
// boot wifi wait — the wifi scan/join flow must be reachable BEFORE the first
// connect, or a device moved to a new place can never be given its network.
static void settingsPump() {
  auto st = M5.Touch.getDetail();
  // Vertical drag scrolls list pages (stations / wifi scan), ~40 px/entry.
  static int dragBase = 0;
  static bool sDragging = false;
  if (st.isPressed()) {
    if (!sDragging && abs(st.distanceY()) > 28 &&
        abs(st.distanceY()) > abs(st.distanceX())) {
      sDragging = true;
      dragBase = 0;
    }
    if (sDragging) {
      int rows = (st.distanceY() - dragBase) / 40;
      if (rows && ui::settingsScroll(rows)) dragBase += rows * 40;
    }
  } else if (sDragging) {
    sDragging = false;
  } else if (st.wasClicked()) {
    ui::SettingsAction action = ui::settingsTouch(st.x, st.y);
    if (action == ui::SettingsAction::ConnectWifi) {
      String ssid = ui::settingsWifiSsid(), pw = ui::settingsWifiPassword();
      bool ok = whwifi::joinNew(ssid, pw, 12000);
      if (ok) {
        whnvs::saveWifi(ssid, pw);
        ESP.restart();  // clean bring-up on the new network via the boot path
      } else {
        whwifi::beginConnect(settings);  // re-kick the stored network
        ui::settingsWifiResult(false);
      }
    } else {
      applySettingsAction(action);
    }
  }
}

// The gesture that opens settings (boot wait + playing screen).
static bool settingsGesture() {
  auto t = M5.Touch.getDetail();
  return M5.BtnB.pressedFor(600) || (t.wasHold() && t.y < 240) ||
         (g_booting && t.wasClicked() && ui::bootGearHit(t.x, t.y));
}

#else  // button board

// Tap vs hold on a physical button. Tap is decided on RELEASE (so a hold can
// mean something else); Hold fires once at the threshold, then Repeat every
// repeatMs while still held (0 = no repeat).
struct ButtonGesture {
  enum Ev : uint8_t { None, Tap, Hold, Repeat };
  m5::Button_Class& btn;
  uint16_t holdMs, repeatMs;
  bool armed = false, held = false;
  uint32_t nextRepeat = 0;

  Ev poll() {
    uint32_t now = millis();
    if (btn.wasPressed()) {
      armed = true;
      held = false;
    }
    if (!armed) return None;  // press began before we were watching
    if (btn.isPressed()) {
      if (!held && btn.pressedFor(holdMs)) {
        held = true;
        nextRepeat = now + repeatMs;
        return Hold;
      }
      if (held && repeatMs && now >= nextRepeat) {
        nextRepeat = now + repeatMs;
        return Repeat;
      }
      return None;
    }
    armed = false;  // released
    return held ? None : Tap;
  }
  void reset() { armed = held = false; }
};

// Button menu: A/C move (instantly on press, repeating on hold), B tap =
// select, B hold = back.
static void settingsPump() {
  static ButtonGesture a{M5.BtnA, 400, 140}, b{M5.BtnB, 600, 0}, c{M5.BtnC, 400, 140};
  auto ea = a.poll(), eb = b.poll(), ec = c.poll();
  ui::SettingsAction action = ui::SettingsAction::None;
  if (M5.BtnA.wasPressed() || ea == ButtonGesture::Hold || ea == ButtonGesture::Repeat)
    action = ui::settingsKey(ui::MenuKey::Up);
  else if (M5.BtnC.wasPressed() || ec == ButtonGesture::Hold || ec == ButtonGesture::Repeat)
    action = ui::settingsKey(ui::MenuKey::Down);
  else if (eb == ButtonGesture::Tap)
    action = ui::settingsKey(ui::MenuKey::Select);
  else if (eb == ButtonGesture::Hold)
    action = ui::settingsKey(ui::MenuKey::Back);
  applySettingsAction(action);
}

// A FRESH B hold (the tracker arms only on a press it saw): closing the menu
// with a B hold must not re-open it while the button is still down.
static bool settingsGesture() {
  static ButtonGesture b{M5.BtnB, 700, 0};
  return b.poll() == ButtonGesture::Hold;
}
#endif  // WH_HAS_TOUCH

#if WH_HAS_TOUCH
// Playing screen, touch. Returns true when it opened settings.
static bool stationInput(const PlayerSnapshot& snap, const m5::touch_detail_t& t) {
  // Settings: hold anywhere on the card ~0.5 s, or hold bezel BtnB.
  if (settingsGesture()) {
    ui::settingsShow(settings.audioOut, settings.brightness);
    return true;
  }

  // Station controls, split by gesture:
  //  - tap (either half) or horizontal flick → change station INSTANTLY, no list.
  //  - vertical drag → browse the toast list; the tune commits when you settle.
  static int dragBaseIdx = -1;
  static bool dragging = false;
  int n = (int)catalog::count();

  if (t.isPressed() && t.y < 240 && n && !dragging && abs(t.distanceY()) > 32 &&
      abs(t.distanceY()) > abs(t.distanceX())) {
    dragging = true;  // vertical drag started → enter browse mode
    dragBaseIdx = snap.stationIndex < 0 ? 0 : snap.stationIndex;
    g_browseIdx = dragBaseIdx;
  }
  if (dragging) {
    if (t.isPressed()) {
      int idx = ((dragBaseIdx - t.distanceY() / 48) % n + n) % n;
      if (idx != g_browseIdx) { g_browseIdx = idx; ui::stationToast(g_browseIdx); }
      g_browseCommitAt = millis() + 600;
    } else {
      dragging = false;  // released — g_browseCommitAt below fires the tune
    }
  } else if (n) {
    // Not a drag: instant prev/next on tap half or horizontal flick. y < 240
    // keeps the bezel button strip (BtnA/B/C live at y >= 240) out of it —
    // without the guard every volume press also registered as a tap here.
    int step = 0;
    if (t.wasFlicked() && t.y < 240 && abs(t.distanceX()) > 30 &&
        abs(t.distanceX()) > abs(t.distanceY())) {
      step = t.distanceX() < 0 ? 1 : -1;
    } else if (t.wasClicked() && t.y < 240 && !g_wakeTouch) {
      // A plain tap that woke a dimmed screen only wakes it (swipes act).
      step = t.x < 160 ? -1 : 1;
    }
    if (step != 0) {
      int cur = snap.stationIndex < 0 ? 0 : snap.stationIndex;
      player::tuneTo(((cur + step) % n + n) % n);
    }
  }
  if (g_browseIdx >= 0 && !dragging && millis() > g_browseCommitAt) {
    player::tuneTo(g_browseIdx);
    g_browseIdx = -1;
  }

  // Bezel buttons: A = vol down, C = vol up. wasPressed (instant) + repeat on
  // hold — wasClicked waits out a multi-click window and felt laggy.
  static uint32_t volRepeatAt = 0;
  int volStep = 0;
  if (M5.BtnA.wasPressed()) volStep = -1;
  if (M5.BtnC.wasPressed()) volStep = 1;
  if ((M5.BtnA.pressedFor(400) || M5.BtnC.pressedFor(400)) && millis() > volRepeatAt) {
    volRepeatAt = millis() + 150;
    volStep = M5.BtnA.isPressed() ? -1 : 1;
  }
  if (volStep) {
    uint8_t vol = snap.volume;
    if (volStep < 0 && vol > 0) vol--;
    if (volStep > 0 && vol < 21) vol++;
    player::setVolume(vol);
    ui::volumeOverlay(vol);
  }
  return false;
}

#else  // button board

static void stepVolume(int dir, uint32_t overlayMs) {
  uint8_t vol = player::snapshot().volume;
  if (dir < 0 && vol > 0) vol--;
  if (dir > 0 && vol < 21) vol++;
  player::setVolume(vol);
  ui::volumeOverlay(vol, overlayMs);
}

// Playing screen, buttons. A/C tap = prev/next (on release, so a hold can
// browse instead); A/C hold = browse the toast, the tune commits ~0.6 s after
// the last step; B tap = volume mode (A/C = -/+ on press, repeat on hold;
// B again or 3 s idle leaves); B hold = settings. Returns true when it
// opened settings.
static bool stationInput(const PlayerSnapshot& snap, const m5::touch_detail_t&) {
  constexpr uint32_t kVolModeMs = 3000;
  static ButtonGesture a{M5.BtnA, 450, 220}, b{M5.BtnB, 700, 0}, c{M5.BtnC, 450, 220};
  static bool volMode = false;
  static uint32_t volModeUntil = 0;
  const auto ea = a.poll(), eb = b.poll(), ec = c.poll();
  const int n = (int)catalog::count();

  if (eb == ButtonGesture::Hold) {
    volMode = false;
    g_browseIdx = -1;
    ui::dismissOverlay();
    ui::settingsShow(settings.audioOut, settings.brightness);
    return true;
  }
  if (eb == ButtonGesture::Tap) {
    volMode = !volMode;
    if (volMode) {
      volModeUntil = millis() + kVolModeMs;
      ui::volumeOverlay(snap.volume, kVolModeMs);
    } else {
      ui::dismissOverlay();
    }
    return false;
  }
  if (volMode) {
    int dir = 0;
    if (M5.BtnA.wasPressed() || ea == ButtonGesture::Hold || ea == ButtonGesture::Repeat) dir = -1;
    if (M5.BtnC.wasPressed() || ec == ButtonGesture::Hold || ec == ButtonGesture::Repeat) dir = 1;
    if (dir) {
      stepVolume(dir, kVolModeMs);
      volModeUntil = millis() + kVolModeMs;
    } else if (millis() > volModeUntil) {
      volMode = false;  // the overlay expires on its own at the same moment
    }
    return false;
  }
  if (!n) return false;

  const int cur = snap.stationIndex < 0 ? 0 : snap.stationIndex;
  int step = 0, browse = 0;
  if (ea == ButtonGesture::Tap) step = -1;
  if (ec == ButtonGesture::Tap) step = 1;
  if (ea == ButtonGesture::Hold || ea == ButtonGesture::Repeat) browse = -1;
  if (ec == ButtonGesture::Hold || ec == ButtonGesture::Repeat) browse = 1;
  if (step && g_browseIdx < 0) {
    player::tuneTo(((cur + step) % n + n) % n);
  } else if (step || browse) {
    // A hold starts a browse at the playing station; taps during a browse
    // keep stepping the list instead of tuning under it.
    int from = g_browseIdx >= 0 ? g_browseIdx : cur;
    g_browseIdx = ((from + (step ? step : browse)) % n + n) % n;
    ui::stationToast(g_browseIdx);
    g_browseCommitAt = millis() + 600;
  }
  if (g_browseIdx >= 0 && !M5.BtnA.isPressed() && !M5.BtnC.isPressed() &&
      millis() > g_browseCommitAt) {
    player::tuneTo(g_browseIdx);
    g_browseIdx = -1;
  }
  return false;
}
#endif  // WH_HAS_TOUCH

void setup() {
  Serial.begin(115200);  // HWCDC: Serial.print* needs this, log_* doesn't
  net::tlsMemInit();     // before anything can open a TLS session
  auto cfg = M5.config();
  cfg.internal_spk = false;  // audio_out owns the amp — keep M5.Speaker off I2S
  cfg.internal_mic = false;  // keep ES7210 off the shared pins
  M5.begin(cfg);
#ifdef WH_PIN_DAC_SPEAKER
  // Unused 8-bit DAC speaker: pin its amp input low (floating = audible hum).
  pinMode(WH_PIN_DAC_SPEAKER, OUTPUT);
  digitalWrite(WH_PIN_DAC_SPEAKER, LOW);
#endif

  whnvs::load(settings);
  ui::begin(settings.brightness);
  ui::bootLine("Waverz\xC2\xB7net  fw %s (build %d)", WH_FW_VERSION, WH_FW_BUILD);

  // Field crash reports arrive as "it rebooted" — name the culprit on the
  // next boot (panic = code bug, brownout = power, wdt = a starved task).
  esp_reset_reason_t rr = esp_reset_reason();
  const char* crash = rr == ESP_RST_PANIC      ? "panic"
                      : rr == ESP_RST_INT_WDT  ? "int-wdt"
                      : rr == ESP_RST_TASK_WDT ? "task-wdt"
                      : rr == ESP_RST_WDT      ? "wdt"
                      : rr == ESP_RST_BROWNOUT ? "brownout"
                                               : nullptr;
  if (crash) ui::bootLine("! prev boot crashed: %s", crash);

  // Partition is labeled "littlefs" (esp_littlefs defaults to "spiffs").
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    ui::bootLine("FS mount failed");
  }
  content_sync::wipeStagingOnBoot();

  // Hold B at power-on = straight into the setup portal (recovery path that
  // needs no menu). Read before anything slow so a quick hold registers.
  M5.update();
  bool portalNow = M5.BtnB.isPressed() || whnvs::takePortalOnBoot();

  ui::bootLine("wifi: connecting ...");
  bool haveCreds = whwifi::beginConnect(settings);
  if (!haveCreds) portalNow = true;  // nothing to try: the phone setup it is
  // Wait for the link, but stay interactive: retry forever AND keep the
  // settings UI (touch: hold the screen / gear; buttons: hold B) reachable so
  // a new network can be joined right here. The stack auto-retries the
  // association by itself (re-calling begin() mid-attempt is rejected with
  // ESP_ERR_WIFI_STATE); settingsPump re-kicks it after the two things that
  // stop it (a scan, a failed join). If the stored network stays unreachable
  // for WH_PORTAL_AFTER_MS the phone portal opens by itself (with an idle
  // timeout, so a router that was merely down still gets retried).
  uint32_t retryAt = millis() + WH_WIFI_TIMEOUT_MS;
  uint32_t failSince = millis();
  auto bootScreenBack = [&]() {
    ui::bootScreen();
    ui::bootLine(haveCreds ? "wifi: connecting ..." : "no wifi saved - phone setup in settings");
    retryAt = millis() + WH_WIFI_TIMEOUT_MS;
  };
  while (!whwifi::isConnected() || ui::settingsOpen()) {
    M5.update();
    serial_cmd::poll();  // boot-safe subset: bench wifi provisioning (wifi-join)
    if (g_portalRequested) {
      g_portalRequested = false;
      portalNow = true;
    }
    if (portalNow && !ui::settingsOpen()) {
      portalNow = false;
      // Returns only on cancel / idle timeout — a join saves + reboots.
      wifi_portal::run(true, haveCreds ? WH_PORTAL_IDLE_MS : 0);
      whwifi::beginConnect(settings);  // back to the stored network (if any)
      failSince = millis();
      bootScreenBack();
    } else if (ui::settingsOpen()) {
      settingsPump();
      if (!ui::settingsOpen() && !g_portalRequested) bootScreenBack();
    } else if (settingsGesture()) {
      ui::settingsShow(settings.audioOut, settings.brightness);
    } else if (haveCreds && millis() - failSince > WH_PORTAL_AFTER_MS) {
      ui::bootLine("wifi: %s unreachable - opening phone setup", settings.ssid.c_str());
      portalNow = true;
    } else if (haveCreds && millis() > retryAt) {
      retryAt = millis() + WH_WIFI_TIMEOUT_MS;
      ui::bootLine("wifi: retrying %s ...", settings.ssid.c_str());
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  whwifi::onLink(settings);
  ui::bootLine("wifi: %s ok", settings.ssid.c_str());

  ui::bootLine("clock: syncing ...");
  bool clockOk = whwifi::syncClock(WH_SNTP_TIMEOUT_MS);
  net::setClockValid(clockOk);
  ui::bootLine(clockOk ? "clock: ok" : "clock: FAILED (no sync/ota/metadata)");

  ui::bootLine("content: checking ...");
  content_sync::run();  // Unchanged/Skipped/Failed all fall through — the
                        // local pack (or a later sweep) keeps us playing
  ui::bootLine("firmware: checking ...");
  fw_update::checkAndUpdate();  // reboots on success

  if (!catalog::load()) {
    ui::bootLine("no content pack!");
    ui::bootLine("run: tools/build.py --seed-m5 && pio run -t uploadfs");
    return;  // loop() idles; device needs content to be a radio
  }
  ui::bootLine("catalog: %u stations (content %.12s)", catalog::count(),
               catalog::contentVersion().c_str());
  ui::preloadIcons();

  // Warm lwIP's DNS cache for the station we're about to tune while the link
  // is known-good (content/fw sync just used it). Measured on-device: lookups
  // issued in the seconds after audio bring-up intermittently time out (6.3 s
  // EAI_FAIL, or a 3 s lost first query), stalling the first tune.
  int start = catalog::indexOfId(settings.lastStation);
  if (start < 0) start = 0;
  if (catalog::count()) {
    const String& url = catalog::at(start).url;
    String host = url.substring(url.indexOf("//") + 2);
    host = host.substring(0, strcspn(host.c_str(), ":/?"));
    IPAddress ip;
    uint32_t t0 = millis();
    bool ok = WiFi.hostByName(host.c_str(), ip) == 1;
    log_i("prewarm dns %s -> %s in %lums", host.c_str(), ok ? ip.toString().c_str() : "FAIL",
          (unsigned long)(millis() - t0));
  }

  bool fellBack = false;
  // Audio output is always auto-detected: Module Audio if present (I2C probe),
  // else the internal amp. (No manual selection — RCA is unprobeable and rare.)
  profile = audio_out::resolve(AudioOutSetting::Auto, fellBack);
  if (!audio_out::init(profile, 48000)) {
    profile = AudioProfile::Internal;
    audio_out::init(profile, 48000);
    fellBack = true;
  }
  ui::bootLine("audio: %s%s", audio_out::name(profile), fellBack ? " (fallback)" : "");

  player::begin(profile, settings.volume, start);
  serial_cmd::setReady();
  g_booting = false;
}

void loop() {
  M5.update();
  player::tick();
  serial_cmd::poll();

  // Settings overlay: modal — BtnB hold opens, taps route to it.
  if (ui::settingsOpen()) {
    settingsPump();
    // A link that drops while the menu sits open must still come back
    // (measured: 150 s offline behind an open Fire menu).
    if (!ui::settingsOwnsRadio()) whwifi::maintain(settings);
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }
  // Auto-dim after inactivity. Waking: a physical button (Fire) brightens
  // AND acts (next while dimmed = wake + next); on touch boards a plain tap
  // on the screen only wakes it, while swipes / drags / bezel buttons act
  // (g_wakeTouch, consumed in stationInput). (No IMU on the SE.)
  auto t = M5.Touch.getDetail();
  static uint32_t lastInteraction = millis();
  static bool dimmed = false;
  bool anyInput = t.isPressed() || M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
                  M5.BtnC.isPressed();
  if (anyInput) {
    lastInteraction = millis();
    if (dimmed) {
      dimmed = false;
      g_wakeTouch = WH_HAS_TOUCH && t.isPressed();
      M5.Display.setBrightness(settings.brightness);
    }
  } else if (!dimmed && millis() - lastInteraction > WH_DIM_AFTER_MS) {
    dimmed = true;
    M5.Display.setBrightness(max<uint8_t>(settings.brightness / WH_DIM_DIVISOR, WH_DIM_MIN));
  }

  PlayerSnapshot snap = player::snapshot();
  bool settingsOpened = stationInput(snap, t);
  if (g_wakeTouch && !t.isPressed()) g_wakeTouch = false;  // waking gesture finished
  if (settingsOpened) {
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  snap = player::snapshot();
  bool stationChanged = snap.stationIndex != lastStationIndex;
  lastStationIndex = snap.stationIndex;

  now_playing::tick(snap.state == PlayerState::Playing, snap.stationIndex, stationChanged);
  NowPlaying np = now_playing::current();

  telemetry::tick(snap.state == PlayerState::Playing,
                  snap.stationIndex >= 0 && snap.stationIndex < (int)catalog::count()
                      ? catalog::at(snap.stationIndex).id
                      : String());

  // While browsing, the toast IS the feedback — rebuilding the card per step
  // (PNG decode from flash each time) caused visible input hitches.
  if (g_browseIdx < 0 &&
      (snap.generation != lastRenderedGen || np.generation != lastNpGen) &&
      snap.stationIndex >= 0) {
    lastRenderedGen = snap.generation;
    lastNpGen = np.generation;
    ui::render(snap, np);
  }

  static uint32_t gaugeAt = 0;
  if (g_browseIdx < 0 && millis() > gaugeAt) {
    gaugeAt = millis() + 1000;
    if (snap.state == PlayerState::Playing) ui::bufferGauge(snap.buffered, snap.bufferTarget);
    ui::wifiMeter(0);  // reads RSSI itself; visible in every state incl. tuning
  }

  if (!ui::settingsOwnsRadio()) whwifi::maintain(settings);

  ui::tick();

  vTaskDelay(pdMS_TO_TICKS(5));
}
