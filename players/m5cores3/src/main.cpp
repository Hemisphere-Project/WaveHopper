// WaveHopper CoreS3 firmware — boot sequencer + input/UI loop.
//
// Boot: display → FS → NVS → wifi → catalog → audio profile → player task →
// auto-play. Content sync + firmware OTA slot in before catalog load (M3).
// Playback policy: no play/pause — the supervisor keeps something playing
// (retry once → skip → sweep). Controls: tap left/right = prev/next station,
// bezel BtnA/BtnC = volume down/up. Settings overlay lands in M4.

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
#include "telemetry.h"
#include "ui.h"

static WhSettings settings;
static AudioProfile profile = AudioProfile::Internal;
static uint32_t lastRenderedGen = 0;
static uint32_t lastNpGen = 0;
static int lastStationIndex = -1;

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
    } else if (action != ui::SettingsAction::None) {
      settings.brightness = ui::settingsBrightness();
      whnvs::saveBrightness(settings.brightness);
      if (action == ui::SettingsAction::CloseAndReboot) ESP.restart();
      // The wifi scan drops an in-progress association to get a clean scan —
      // make sure the stored network is trying again once settings closes.
      if (!whwifi::isConnected()) whwifi::beginConnect(settings);
    }
  }
}

void setup() {
  Serial.begin(115200);  // HWCDC: Serial.print* needs this, log_* doesn't
  auto cfg = M5.config();
  cfg.internal_spk = false;  // audio_out owns the amp — keep M5.Speaker off I2S
  cfg.internal_mic = false;  // keep ES7210 off the shared pins
  M5.begin(cfg);

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

  ui::bootLine("wifi: connecting ...");
  bool haveCreds = whwifi::beginConnect(settings);
  if (!haveCreds) ui::bootLine("no wifi saved - tap gear to set up");
  // Wait for the link, but stay interactive: retry forever AND keep the
  // settings overlay (hold the screen / BtnB) reachable so a new network can
  // be joined right here. The stack auto-retries the association by itself
  // (re-calling begin() mid-attempt is rejected with ESP_ERR_WIFI_STATE);
  // settingsPump re-kicks it after the two things that stop it (a scan, a
  // failed join). The periodic line is progress feedback only.
  uint32_t retryAt = millis() + WH_WIFI_TIMEOUT_MS;
  while (!whwifi::isConnected() || ui::settingsOpen()) {
    M5.update();
    auto t = M5.Touch.getDetail();
    if (ui::settingsOpen()) {
      settingsPump();
      if (!ui::settingsOpen()) {  // closed without joining — boot screen back
        ui::bootScreen();
        ui::bootLine(haveCreds ? "wifi: connecting ..."
                               : "no wifi saved - tap gear to set up");
        retryAt = millis() + WH_WIFI_TIMEOUT_MS;
      }
    } else if (M5.BtnB.pressedFor(600) || (t.wasHold() && t.y < 240) ||
               (t.wasClicked() && ui::bootGearHit(t.x, t.y))) {
      ui::settingsShow(settings.audioOut, settings.brightness);
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

  int start = catalog::indexOfId(settings.lastStation);
  player::begin(profile, settings.volume, start >= 0 ? start : 0);
}

void loop() {
  M5.update();
  player::tick();

  // Settings overlay: modal — BtnB hold opens, taps route to it.
  if (ui::settingsOpen()) {
    settingsPump();
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }
  // Auto-dim after inactivity; the waking touch is swallowed so it never
  // changes station by accident. (No IMU on the SE — touch wake only.)
  auto t = M5.Touch.getDetail();
  static uint32_t lastInteraction = millis();
  static bool dimmed = false;
  static bool wakeSwallow = false;
  bool anyInput = t.isPressed() || M5.BtnA.isPressed() || M5.BtnB.isPressed() ||
                  M5.BtnC.isPressed();
  if (anyInput) {
    lastInteraction = millis();
    if (dimmed) {
      dimmed = false;
      wakeSwallow = true;
      M5.Display.setBrightness(settings.brightness);
    }
  } else if (!dimmed && millis() - lastInteraction > WH_DIM_AFTER_MS) {
    dimmed = true;
    M5.Display.setBrightness(max<uint8_t>(settings.brightness / 4, 12));
  }
  if (wakeSwallow) {
    if (!anyInput) wakeSwallow = false;  // gesture over — resume input handling
    ui::tick();
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  // Settings: hold anywhere on the card ~0.5 s, or hold bezel BtnB.
  if (M5.BtnB.pressedFor(600) || (t.wasHold() && t.y < 240)) {
    ui::settingsShow(settings.audioOut, settings.brightness);
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  PlayerSnapshot snap = player::snapshot();

  // Station controls, split by gesture:
  //  - tap (either half) or horizontal flick → change station INSTANTLY, no list.
  //  - vertical drag → browse the toast list; the tune commits when you settle.
  static int browseIdx = -1;         // >=0 only during a vertical-drag browse
  static uint32_t browseCommitAt = 0;
  static int dragBaseIdx = -1;
  static bool dragging = false;
  int n = (int)catalog::count();

  if (t.isPressed() && t.y < 240 && n && !dragging && abs(t.distanceY()) > 32 &&
      abs(t.distanceY()) > abs(t.distanceX())) {
    dragging = true;  // vertical drag started → enter browse mode
    dragBaseIdx = snap.stationIndex < 0 ? 0 : snap.stationIndex;
    browseIdx = dragBaseIdx;
  }
  if (dragging) {
    if (t.isPressed()) {
      int idx = ((dragBaseIdx - t.distanceY() / 48) % n + n) % n;
      if (idx != browseIdx) { browseIdx = idx; ui::stationToast(browseIdx); }
      browseCommitAt = millis() + 600;
    } else {
      dragging = false;  // released — browseCommitAt below fires the tune
    }
  } else if (n) {
    // Not a drag: instant prev/next on tap half or horizontal flick. y < 240
    // keeps the bezel button strip (BtnA/B/C live at y >= 240) out of it —
    // without the guard every volume press also registered as a tap here.
    int step = 0;
    if (t.wasFlicked() && t.y < 240 && abs(t.distanceX()) > 30 &&
        abs(t.distanceX()) > abs(t.distanceY())) {
      step = t.distanceX() < 0 ? 1 : -1;
    } else if (t.wasClicked() && t.y < 240) {
      step = t.x < 160 ? -1 : 1;
    }
    if (step != 0) {
      int cur = snap.stationIndex < 0 ? 0 : snap.stationIndex;
      player::tuneTo(((cur + step) % n + n) % n);
    }
  }
  if (browseIdx >= 0 && !dragging && millis() > browseCommitAt) {
    player::tuneTo(browseIdx);
    browseIdx = -1;
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
  if (browseIdx < 0 &&
      (snap.generation != lastRenderedGen || np.generation != lastNpGen) &&
      snap.stationIndex >= 0) {
    lastRenderedGen = snap.generation;
    lastNpGen = np.generation;
    ui::render(snap, np);
  }

  static uint32_t gaugeAt = 0;
  if (browseIdx < 0 && millis() > gaugeAt) {
    gaugeAt = millis() + 1000;
    if (snap.state == PlayerState::Playing) ui::bufferGauge(snap.buffered, snap.bufferTarget);
    ui::wifiMeter(0);  // reads RSSI itself; visible in every state incl. tuning
  }

  ui::tick();

  vTaskDelay(pdMS_TO_TICKS(5));
}
