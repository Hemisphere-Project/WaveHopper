#include "player.h"

#include <Audio.h>
#include <WiFi.h>

#include <algorithm>
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
// Adaptive prebuffer: stations that repeatedly drain their cushion earn a
// bigger one (per session). Grows 1.5x per chronic-low event, capped. Observed
// supply pattern (Airtime + this wifi): macro-bursts with 10-20 s sub-realtime
// stretches — cushions converge around 200-350 KB. Lib buffer is 640 KB total.
std::vector<uint32_t> g_targets;
constexpr uint32_t kTargetCap = 393216;  // ~16 s at 192 kbps
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
    case Audio::evt_log:
      log_w("[audio:log] %s", m.msg ? m.msg : "");
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
  g_targets.assign(catalog::count(), WH_PREBUFFER_BYTES);

  Audio::audio_info_callback = onAudioEvent;
  AudioPins p = audio_out::pins(profile);
  g_audio.setPinout(p.bclk, p.lrck, p.dout, p.mclk);
  // Core split (the lib requires decode and audio.loop() on OPPOSITE cores):
  //   core 0 = playerTask (network/TLS pump + wifi stack)
  //   core 1 = decode task + UI loopTask
  // With both on core 0, HLS's bursty per-segment TLS fetches starved the
  // decoder → glitchy segment audio; Icecast's steady trickle didn't show it.
  g_audio.setAudioTaskCore(1);
  // Constant 48 kHz output clock: the AW88298 can't lock the ESP32's
  // fractional 44.1 kHz BCLK and faults on I2S clock reconfigs (see M0 notes).
  g_audio.setOutput48KHz(true);
  g_audio.setVolume(g_volume);

  g_decodeTask = xTaskGetHandle("PeriodicTask");  // created by setPinout above
  if (!g_decodeTask) log_e("decode task not found — prebuffering disabled");

  g_cmdQueue = xQueueCreate(8, sizeof(Cmd));
  // 10 KB: connecttohost runs a TLS handshake (unverified, but still mbedtls)
  // on this stack. Pinned to core 0: during the prebuffer burst the stream's
  // TLS decrypt saturates this task at prio 2 — on core 1 that froze the UI
  // task for the whole buffering window.
  xTaskCreatePinnedToCore(playerTask, "wh_player", 10240, nullptr, 2, nullptr, 0);

  if (firstStationIndex >= 0 && firstStationIndex < (int)catalog::count()) {
    startTune(firstStationIndex, 1);
  } else if (catalog::count() > 0) {
    startTune(0, 1);
  }
}

void tuneTo(int index) {
  if (index < 0 || index >= (int)catalog::count()) return;
  if (index == g_current && g_state == PlayerState::Playing) return;  // settled in place
  manualTune(index);
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
      uint32_t adaptive = g_targets.empty() ? WH_PREBUFFER_BYTES : g_targets[g_current];
      // HLS caps its cushion at the CDN DVR window — target what's reachable so
      // startup ends on the initial burst, not the deadline.
      if (catalog::at(g_current).isHls) adaptive = WH_PREBUFFER_HLS_BYTES;
      uint32_t targetBytes = g_tuneFast ? WH_PREBUFFER_FAST_BYTES : adaptive;
      // Scale the wait cap with the adaptive target (paced servers need time).
      uint32_t targetWait = g_tuneFast
                                ? WH_PREBUFFER_FAST_WAIT_MS
                                : std::min<uint32_t>(12000, (uint64_t)WH_PREBUFFER_WAIT_MS *
                                                                adaptive / WH_PREBUFFER_BYTES);
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
        startTune(g_current, 1);  // full retry budget — don't rush to skip
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
        // low watermark scales with the learned target so a post-RF-outage
        // shallow cushion recovers promptly (a paced server would otherwise
        // never refill it). Cumulative-in-window so brief dips don't fire;
        // min spacing so a dead link doesn't thrash reconnects.
        //
        // HLS live streams are the exception: arrival is capped near realtime
        // at the live edge, so a reconnect can't out-run consumption — it just
        // resets to the edge and re-bursts the playlist, INTERRUPTING the CDN's
        // natural burst recovery (measured: The Lot self-heals 100↔137 KB).
        // For HLS we only rebuffer at near-stall, and never grow the target.
        static uint32_t lowAccumMs = 0, windowStart = 0, lastLowTick = 0,
                        lastRebuffer = 0;
        bool hls = catalog::at(g_current).isHls;
        uint32_t target = g_targets.empty() ? WH_PREBUFFER_BYTES : g_targets[g_current];
        uint32_t lowWater = hls ? WH_REBUFFER_LOW
                                : std::max<uint32_t>(WH_REBUFFER_LOW, target / 4);
        if (!windowStart || now - windowStart > WH_REBUFFER_WINDOW_MS) {
          windowStart = now;
          lowAccumMs = 0;
        }
        if (buffered < lowWater && lastLowTick) lowAccumMs += now - lastLowTick;
        lastLowTick = now;
        if (lowAccumMs > WH_REBUFFER_LOW_MS && now - lastRebuffer > 15000) {
          lowAccumMs = 0;
          windowStart = 0;
          lastRebuffer = now;
          if (!g_targets.empty() && !hls && buffered < WH_REBUFFER_LOW) {
            // Deep depletion (not just shallow-after-recovery): the station
            // earns a bigger cushion. Not for HLS — its ceiling is the CDN.
            g_targets[g_current] =
                std::min<uint32_t>(kTargetCap, g_targets[g_current] * 3 / 2);
          }
          log_e("cushion low on [%d] (%lu, water %lu) — rebuffering, target %lu",
                g_current, (unsigned long)buffered, (unsigned long)lowWater,
                (unsigned long)(g_targets.empty() ? 0 : g_targets[g_current]));
          startTune(g_current, 1);
          break;
        }
      }
      if (now - g_lastHealth > 5000) {
        g_lastHealth = now;
        audio_out::onSampleRate(g_profile, 48000);  // SWS fault self-heal
      }
      {  // buffer diagnostics: 10 s cadence, min tracks the worst dip.
        // arrival = Δbuffer + consumption — separates supply-side problems
        // (arrival < bitrate: wifi/server) from consumption-side ones.
        static uint32_t lastStat = 0, minBuf = UINT32_MAX, lastBuf = 0;
        uint32_t bufNow = g_audio.inBufferFilled();
        minBuf = std::min(minBuf, bufNow);
        if (now - lastStat >= 10000) {
          float dt = (now - lastStat) / 1000.0f;
          float consKBs = g_audio.getBitRate() / 8.0f / 1000.0f;
          float arrivKBs = consKBs + ((int32_t)bufNow - (int32_t)lastBuf) / dt / 1000.0f;
          lastStat = now;
          log_i("[buf] now=%lu min=%lu target=%lu cons=%.1fKB/s arriv=%.1fKB/s "
                "rssi=%d sleep=%d heap=%lu maxblk=%lu",
                (unsigned long)bufNow, (unsigned long)minBuf,
                (unsigned long)(g_targets.empty() ? 0 : g_targets[g_current]),
                consKBs, arrivKBs, WiFi.RSSI(), (int)WiFi.getSleep(),
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());
          minBuf = UINT32_MAX;
          lastBuf = bufNow;
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
  s.buffered = g_audio.inBufferFilled();
  s.bufferTarget = (g_current >= 0 && !g_targets.empty()) ? g_targets[g_current]
                                                          : WH_PREBUFFER_BYTES;
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
