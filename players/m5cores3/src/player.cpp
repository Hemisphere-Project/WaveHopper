#include "player.h"

#include <Audio.h>
#include <WiFi.h>

#include <atomic>
#include <vector>

#include "catalog.h"
#include "config.h"
#include "wh_nvs.h"

namespace {

Audio g_audio(I2S_NUM_1);  // same port M5Unified drives the amp with
AudioProfile g_profile = AudioProfile::Internal;
QueueHandle_t g_cmdQueue = nullptr;
// The lib's decode task. We suspend it across connect so loop()'s network
// pump can build the prebuffer cushion before playback starts (the lib has no
// prebuffer API; decode otherwise starts at ~2 KB buffered).
TaskHandle_t g_decodeTask = nullptr;
std::atomic<bool> g_decodeSuspended{false};

struct Cmd {
  enum Type : uint8_t { Tune, Volume, Stop } type;
  int index;
  uint8_t vol;
};

// Signals from the audio callback (player-task context) to the supervisor
// (UI-task context). Simple idempotent flags — no queue needed.
std::atomic<bool> g_evtEof{false};
std::atomic<bool> g_evtConnectFail{false};
std::atomic<uint32_t> g_titleGen{0};
portMUX_TYPE g_titleMux = portMUX_INITIALIZER_UNLOCKED;
char g_streamTitle[160] = "";

// Supervisor state — touched from the UI task only.
PlayerState g_state = PlayerState::Idle;
int g_current = -1;
uint8_t g_attempt = 0;
bool g_tuneFast = false;  // manual surf: short prebuffer for fast feedback
uint32_t g_deadline = 0;
std::vector<bool> g_failedSweep;
uint32_t g_sweepAt = 0;
uint32_t g_stallSince = 0;
uint32_t g_lastHealth = 0;
uint8_t g_volume = 12;
bool g_volDirty = false;
uint32_t g_volSaveAt = 0;
uint32_t g_gen = 1;

void onAudioEvent(Audio::msg_t m) {  // player-task context (audio.loop drain)
  switch (m.e) {
    case Audio::evt_streamtitle:
      portENTER_CRITICAL(&g_titleMux);
      strlcpy(g_streamTitle, m.msg ? m.msg : "", sizeof(g_streamTitle));
      portEXIT_CRITICAL(&g_titleMux);
      g_titleGen++;
      break;
    case Audio::evt_eof:
      g_evtEof = true;
      break;
    case Audio::evt_info:
      log_i("[audio] %s", m.msg ? m.msg : "");
      break;
    default:
      break;
  }
}

void clearTitle() {
  portENTER_CRITICAL(&g_titleMux);
  g_streamTitle[0] = '\0';
  portEXIT_CRITICAL(&g_titleMux);
  g_titleGen++;
}

void playerTask(void*) {
  for (;;) {
    // Drain the queue, coalescing rapid station surfing to the newest tune so
    // a blocking connect never executes stale work.
    Cmd cmd;
    bool haveTune = false, haveStop = false;
    Cmd tuneCmd{};
    while (xQueueReceive(g_cmdQueue, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case Cmd::Tune:   haveTune = true; haveStop = false; tuneCmd = cmd; break;
        case Cmd::Stop:   haveStop = true; haveTune = false; break;
        case Cmd::Volume: g_audio.setVolume(cmd.vol); break;
      }
    }
    if (haveStop) {
      if (g_decodeSuspended.exchange(false)) vTaskResume(g_decodeTask);
      g_audio.stopSong();
      clearTitle();
    }
    if (haveTune) {
      // Resume → stop → suspend: the decode task idles once m_f_running drops,
      // so suspending here can't park it while it holds anything hot.
      if (g_decodeSuspended.exchange(false)) vTaskResume(g_decodeTask);
      g_audio.stopSong();
      clearTitle();
      if (g_decodeTask) {
        vTaskSuspend(g_decodeTask);
        g_decodeSuspended = true;
      }
      const Station& s = catalog::at(tuneCmd.index);
      log_i("tuning [%d] %s -> %s", tuneCmd.index, s.id.c_str(), s.url.c_str());
      if (!g_audio.connecttohost(s.url.c_str())) g_evtConnectFail = true;
    }
    g_audio.loop();  // all lib networking happens here
    vTaskDelay(1);
  }
}

void startTune(int index, uint8_t attempt, bool fast = false) {
  g_current = index;
  g_attempt = attempt;
  g_tuneFast = fast;
  g_state = PlayerState::Tuning;
  g_deadline = millis() + WH_TUNE_TIMEOUT_MS;
  g_stallSince = 0;
  g_evtEof = false;
  g_evtConnectFail = false;
  Cmd c{Cmd::Tune, index, 0};
  xQueueSend(g_cmdQueue, &c, 0);
  g_gen++;
}

void sendStop() {
  Cmd c{Cmd::Stop, 0, 0};
  xQueueSend(g_cmdQueue, &c, 0);
}

void clearSweep() { std::fill(g_failedSweep.begin(), g_failedSweep.end(), false); }

// One retry on the same station, then mark it failed and advance; when every
// station is marked, give up until the next sweep. Mirrors the webapp.
void failCurrent() {
  if (g_attempt < 2) {
    startTune(g_current, 2);
    return;
  }
  g_failedSweep[g_current] = true;
  size_t n = catalog::count();
  for (size_t k = 1; k <= n; ++k) {
    int idx = (g_current + (int)k) % (int)n;
    if (!g_failedSweep[idx]) {
      startTune(idx, 1);
      return;
    }
  }
  sendStop();
  g_state = PlayerState::AllFailed;
  g_sweepAt = millis() + WH_ALLFAIL_SWEEP_MS;
  g_gen++;
}

void manualTune(int index) {
  clearSweep();  // user intent resets the failure sweep
  startTune(index, 1, /*fast=*/true);
}

}  // namespace

namespace player {

void begin(AudioProfile profile, uint8_t volume, int firstStationIndex) {
  g_profile = profile;
  g_volume = volume > 21 ? 21 : volume;
  g_failedSweep.assign(catalog::count(), false);

  Audio::audio_info_callback = onAudioEvent;
  AudioPins p = audio_out::pins(profile);
  g_audio.setPinout(p.bclk, p.lrck, p.dout, p.mclk);
  // Constant 48 kHz output clock: the AW88298 can't lock the ESP32's
  // fractional 44.1 kHz BCLK and faults on I2S clock reconfigs (see M0 notes).
  g_audio.setOutput48KHz(true);
  g_audio.setVolume(g_volume);

  g_decodeTask = xTaskGetHandle("PeriodicTask");  // created by setPinout above
  if (!g_decodeTask) log_e("decode task not found — prebuffering disabled");

  g_cmdQueue = xQueueCreate(8, sizeof(Cmd));
  // 10 KB: connecttohost runs a TLS handshake (unverified, but still mbedtls)
  // on this stack.
  xTaskCreatePinnedToCore(playerTask, "wh_player", 10240, nullptr, 2, nullptr, 1);

  if (firstStationIndex >= 0 && firstStationIndex < (int)catalog::count()) {
    startTune(firstStationIndex, 1);
  } else if (catalog::count() > 0) {
    startTune(0, 1);
  }
}

void next() {
  if (!catalog::count()) return;
  manualTune(((g_current < 0 ? 0 : g_current) + 1) % (int)catalog::count());
}

void prev() {
  if (!catalog::count()) return;
  int n = (int)catalog::count();
  manualTune(((g_current < 0 ? 0 : g_current) - 1 + n) % n);
}

void setVolume(uint8_t v) {
  g_volume = v > 21 ? 21 : v;
  Cmd c{Cmd::Volume, 0, g_volume};
  xQueueSend(g_cmdQueue, &c, 0);
  g_volDirty = true;
  g_volSaveAt = millis() + 1500;  // debounce NVS wear
  g_gen++;
}

void tick() {
  uint32_t now = millis();

  static uint32_t lastWifiCheck = 0;
  if (now - lastWifiCheck > 1000) {
    lastWifiCheck = now;
    bool up = WiFi.status() == WL_CONNECTED;
    if (!up && g_state != PlayerState::WifiLost && g_state != PlayerState::Idle) {
      sendStop();
      g_state = PlayerState::WifiLost;
      g_gen++;
    } else if (up && g_state == PlayerState::WifiLost) {
      clearSweep();
      startTune(g_current >= 0 ? g_current : 0, 1);
    }
  }

  switch (g_state) {
    case PlayerState::Tuning: {
      if (g_evtConnectFail.exchange(false) || g_evtEof.exchange(false)) {
        failCurrent();
        break;
      }
      uint32_t buffered = g_audio.inBufferFilled();
      uint32_t tunedFor = WH_TUNE_TIMEOUT_MS - (g_deadline - now);
      uint32_t targetBytes = g_tuneFast ? WH_PREBUFFER_FAST_BYTES : WH_PREBUFFER_BYTES;
      uint32_t targetWait = g_tuneFast ? WH_PREBUFFER_FAST_WAIT_MS : WH_PREBUFFER_WAIT_MS;
      bool cushionDone = buffered >= targetBytes ||
                         (tunedFor > targetWait && buffered > 8192);
      if (g_audio.isRunning() && cushionDone) {
        if (g_decodeSuspended.exchange(false)) vTaskResume(g_decodeTask);
        log_i("playing [%d] with %lu bytes cushion", g_current, (unsigned long)buffered);
        g_state = PlayerState::Playing;
        clearSweep();
        whnvs::saveLastStation(catalog::at(g_current).id);
        audio_out::onSampleRate(g_profile, 48000);  // amp healthy at stream start
        g_lastHealth = now;
        g_gen++;
      } else if (now > g_deadline) {
        log_e("tune deadline hit on [%d]", g_current);
        failCurrent();
      }
      break;
    }

    case PlayerState::Playing:
      if (g_evtEof.exchange(false) || !g_audio.isRunning()) {
        log_e("stream dropped on [%d]", g_current);
        startTune(g_current, 2);  // one retry, then failCurrent skips
        break;
      }
      {
        uint32_t buffered = g_audio.inBufferFilled();
        if (buffered == 0) {
          if (!g_stallSince) {
            g_stallSince = now;
          } else if (now - g_stallSince > WH_STALL_MS) {
            log_e("stall on [%d] (buffer empty %lus)", g_current, WH_STALL_MS / 1000);
            startTune(g_current, 2);
            break;
          }
        } else {
          g_stallSince = 0;
        }
        // Depleted cushion → one deliberate reconnect (burst-on-connect or a
        // fresh CDN edge rebuilds it) instead of sawtooth gap-crackle. The
        // trigger is cumulative low time in a rolling window: brief dips that
        // self-recover don't fire; a chronic sawtooth does.
        static uint32_t lowAccumMs = 0, windowStart = 0, lastLowTick = 0;
        if (!windowStart || now - windowStart > WH_REBUFFER_WINDOW_MS) {
          windowStart = now;
          lowAccumMs = 0;
        }
        if (buffered < WH_REBUFFER_LOW && lastLowTick) lowAccumMs += now - lastLowTick;
        lastLowTick = now;
        if (lowAccumMs > WH_REBUFFER_LOW_MS) {
          lowAccumMs = 0;
          windowStart = 0;
          log_e("chronic low buffer on [%d] (%lu now) — rebuffering", g_current,
                (unsigned long)buffered);
          startTune(g_current, 2);
          break;
        }
      }
      if (now - g_lastHealth > 5000) {
        g_lastHealth = now;
        audio_out::onSampleRate(g_profile, 48000);  // SWS fault self-heal
      }
      {  // pipeline stats — visible when built with CORE_DEBUG_LEVEL>=4
        static uint32_t lastStat = 0;
        if (now - lastStat >= 5000) {
          lastStat = now;
          log_d("[stat] buf=%lu heap=%lu", (unsigned long)g_audio.inBufferFilled(),
                (unsigned long)ESP.getFreeHeap());
        }
      }
      break;

    case PlayerState::AllFailed:
      if (now > g_sweepAt) {
        clearSweep();
        startTune(g_current >= 0 ? g_current : 0, 1);
      }
      break;

    default:
      break;
  }

  if (g_volDirty && now > g_volSaveAt) {
    g_volDirty = false;
    whnvs::saveVolume(g_volume);
  }
}

PlayerSnapshot snapshot() {
  PlayerSnapshot s;
  s.state = g_state;
  s.stationIndex = g_current;
  s.volume = g_volume;
  s.generation = g_gen + g_titleGen.load();
  portENTER_CRITICAL(&g_titleMux);
  strlcpy(s.streamTitle, g_streamTitle, sizeof(s.streamTitle));
  portEXIT_CRITICAL(&g_titleMux);
  return s;
}

const char* stateName(PlayerState s) {
  switch (s) {
    case PlayerState::Idle:      return "idle";
    case PlayerState::Tuning:    return "tuning";
    case PlayerState::Playing:   return "playing";
    case PlayerState::AllFailed: return "no stations on air";
    case PlayerState::WifiLost:  return "wifi lost";
  }
  return "?";
}

}  // namespace player
