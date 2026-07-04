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

void setup() {
  Serial.begin(115200);  // HWCDC: Serial.print* needs this, log_* doesn't
  auto cfg = M5.config();
  cfg.internal_spk = false;  // audio_out owns the amp — keep M5.Speaker off I2S
  cfg.internal_mic = false;  // keep ES7210 off the shared pins
  M5.begin(cfg);

  whnvs::load(settings);
  ui::begin(settings.brightness);
  ui::bootLine("WaveHopper  fw %s (build %d)", WH_FW_VERSION, WH_FW_BUILD);

  // Partition is labeled "littlefs" (esp_littlefs defaults to "spiffs").
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    ui::bootLine("FS mount failed");
  }
  content_sync::wipeStagingOnBoot();

  ui::bootLine("wifi: connecting ...");
  while (!whwifi::connect(settings, WH_WIFI_TIMEOUT_MS)) {
    ui::bootLine("wifi failed - retrying (check secrets.h?)");
    delay(5000);
  }
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
          whwifi::connect(settings, WH_WIFI_TIMEOUT_MS);  // restore old link
          ui::settingsWifiResult(false);
        }
      } else if (action != ui::SettingsAction::None) {
        settings.brightness = ui::settingsBrightness();
        whnvs::saveBrightness(settings.brightness);
        if (action == ui::SettingsAction::CloseAndReboot) ESP.restart();
      }
    }
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

  // Station browsing: taps/flicks move a *display* selection through the
  // toast; the actual tune fires only once the selection settles — rapid
  // next-next-next never queues blocking connects.
  static int browseIdx = -1;
  static uint32_t browseCommitAt = 0;
  int n = (int)catalog::count();
  int step = 0;
  if (t.y < 240) {
    if (t.wasFlicked() && abs(t.distanceX()) > 30 &&
        abs(t.distanceX()) > abs(t.distanceY())) {
      step = t.distanceX() < 0 ? 1 : -1;
    } else if (t.wasClicked()) {
      step = t.x < 160 ? -1 : 1;
    }
  }
  if (step && n) {
    if (browseIdx < 0) browseIdx = snap.stationIndex < 0 ? 0 : snap.stationIndex;
    browseIdx = (browseIdx + step + n) % n;
    browseCommitAt = millis() + 600;
    ui::stationToast(browseIdx);
  }
  // Vertical drag scrolls the selection continuously (~48 px per station);
  // drag down moves up the list, like scrolling content.
  static int dragBaseIdx = -1;
  static bool dragging = false;
  if (t.isPressed() && t.y < 240 && n) {
    int dy = t.distanceY();
    if (!dragging && abs(dy) > 32 && abs(dy) > abs(t.distanceX()) * 2) {
      dragging = true;
      dragBaseIdx = browseIdx >= 0 ? browseIdx
                                   : (snap.stationIndex < 0 ? 0 : snap.stationIndex);
    }
    if (dragging) {
      int idx = ((dragBaseIdx - dy / 48) % n + n) % n;
      if (idx != browseIdx) {
        browseIdx = idx;
        ui::stationToast(browseIdx);
      }
      browseCommitAt = millis() + 600;
    }
  } else if (dragging) {
    dragging = false;
  }
  if (browseIdx >= 0 && millis() > browseCommitAt) {
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
