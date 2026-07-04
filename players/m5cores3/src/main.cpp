// WaveHopper CoreS3 firmware — boot sequencer + input/UI loop.
//
// Boot: display → FS → NVS → wifi → catalog → audio profile → player task →
// auto-play. Content sync + firmware OTA slot in before catalog load (M3).
// Playback policy: no play/pause — the supervisor keeps something playing
// (retry once → skip → sweep). Controls: tap left/right = prev/next station,
// bezel BtnA/BtnC = volume down/up. Settings overlay lands in M4.

#include <M5Unified.h>
#include <LittleFS.h>

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
  profile = audio_out::resolve(settings.audioOut, fellBack);
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
    if (st.wasClicked()) {
      ui::SettingsAction action = ui::settingsTouch(st.x, st.y);
      if (action != ui::SettingsAction::None) {
        settings.brightness = ui::settingsBrightness();
        whnvs::saveBrightness(settings.brightness);
        if (action == ui::SettingsAction::CloseAndReboot) {
          whnvs::saveAudioOut(ui::settingsAudioOut());
          ESP.restart();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }
  if (M5.BtnB.pressedFor(600)) {
    ui::settingsShow(settings.audioOut, settings.brightness);
    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  // Touch: left/right half tap = prev/next; horizontal flick = next/prev
  // (flick left → next, mirroring the webapp swipe).
  auto t = M5.Touch.getDetail();
  if (t.y < 240) {
    if (t.wasFlicked() && abs(t.distanceX()) > 30 &&
        abs(t.distanceX()) > abs(t.distanceY())) {
      log_d("[input] flick dx=%d", t.distanceX());
      if (t.distanceX() < 0) player::next();
      else player::prev();
    } else if (t.wasClicked()) {
      log_d("[input] tap x=%d", t.x);
      if (t.x < 160) player::prev();
      else player::next();
    }
  }
  // Bezel buttons: A = vol down, C = vol up.
  bool volChanged = false;
  PlayerSnapshot snap = player::snapshot();
  uint8_t vol = snap.volume;
  if (M5.BtnA.wasClicked() && vol > 0)  { vol--; volChanged = true; }
  if (M5.BtnC.wasClicked() && vol < 21) { vol++; volChanged = true; }
  if (volChanged) {
    player::setVolume(vol);
    ui::volumeOverlay(vol);
  }

  snap = player::snapshot();
  bool stationChanged = snap.stationIndex != lastStationIndex;
  if (stationChanged && lastStationIndex >= 0) ui::stationToast(snap.stationIndex);
  lastStationIndex = snap.stationIndex;

  now_playing::tick(snap.state == PlayerState::Playing, snap.stationIndex, stationChanged);
  NowPlaying np = now_playing::current();

  if ((snap.generation != lastRenderedGen || np.generation != lastNpGen) &&
      snap.stationIndex >= 0) {
    lastRenderedGen = snap.generation;
    lastNpGen = np.generation;
    ui::render(snap, np);
  }
  ui::tick();

  vTaskDelay(pdMS_TO_TICKS(5));
}
