// WaveHopper CoreS3 — M0 audio spike, DIAGNOSTIC build.
//
// Phase 1: prove the amp hardware via M5Unified's own path (M5.Speaker tone),
//          dumping the amp register state it leaves behind.
// Phase 2: hand I2S to ESP32-audioI2S, re-init the amp ourselves, stream, and
//          dump the same registers + AW88298 SYSST (does the amp see BCLK?).

#include <M5Unified.h>
#include <Audio.h>
#include <LittleFS.h>

#include "config.h"
#include "wh_nvs.h"
#include "wh_wifi.h"
#include "audio_out.h"

static const char* kSpikeUrl = "https://stream-relay-geo.ntslive.net/stream";

static Audio audio(I2S_NUM_1);  // same port M5Unified drives the amp with
static WhSettings settings;
static AudioProfile profile = AudioProfile::Internal;
static uint32_t lastRate = 0;

static uint16_t awRead(uint8_t reg) {
  uint8_t buf[2] = {0, 0};
  M5.In_I2C.readRegister(WH_I2C_AW88298, reg, buf, 2, 400000);
  return (uint16_t(buf[0]) << 8) | buf[1];  // print both orders below anyway
}

static void dumpAmp(const char* tag) {
  Serial.printf("[diag:%s] AW88298 id=%04x sysst=%04x sysctrl(04)=%04x "
                "sysctrl2(05)=%04x i2sctrl(06)=%04x hagc(0C)=%04x\n",
                tag, awRead(0x00), awRead(0x01), awRead(0x04), awRead(0x05),
                awRead(0x06), awRead(0x0C));
  uint8_t aw9523_02 = 0;
  M5.In_I2C.readRegister(WH_I2C_AW9523, 0x02, &aw9523_02, 1, 400000);
  Serial.printf("[diag:%s] AW9523 reg02=%02x (bit2=%d)\n", tag, aw9523_02,
                (aw9523_02 >> 2) & 1);
}

static void onAudioEvent(Audio::msg_t m) {
  if (m.e == Audio::evt_info || m.e == Audio::evt_eof)
    log_i("[audio] %s", m.msg ? m.msg : "");
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.internal_spk = true;  // phase 1: let M5Unified own the amp for the beep
  cfg.internal_mic = false;
  M5.begin(cfg);
  M5.Display.setTextSize(2);
  M5.Display.setTextScroll(true);
  M5.Display.println("M0 DIAG");
  delay(1000);

  dumpAmp("boot");

  Serial.println("[diag] phase1: M5.Speaker tone 440Hz 2s");
  M5.Display.println("beep test...");
  M5.Speaker.setVolume(160);
  M5.Speaker.tone(440, 2000);
  delay(500);
  dumpAmp("m5spk-on");   // register state while M5Unified's init is live
  delay(2000);
  M5.Speaker.end();       // fires the disable cb (amp off, rail off)
  dumpAmp("m5spk-off");

  Serial.println("[diag] phase2: audioI2S path");
  M5.Display.println("audioI2S test...");
  whnvs::load(settings);

  if (!whwifi::connect(settings, WH_WIFI_TIMEOUT_MS)) {
    Serial.println("[diag] wifi FAILED");
    return;
  }

  audio_out::init(AudioProfile::Internal, 44100);
  dumpAmp("wh-init");     // compare against m5spk-on

  Audio::audio_info_callback = onAudioEvent;
  AudioPins p = audio_out::pins(profile);
  bool pinoutOk = audio.setPinout(p.bclk, p.lrck, p.dout, p.mclk);
  Serial.printf("[diag] setPinout(%d,%d,%d,%d) -> %s\n", p.bclk, p.lrck, p.dout,
                p.mclk, pinoutOk ? "OK" : "FAILED");
  // Pin the I2S output clock at 48 kHz (lib resamples): the AW88298 never
  // locks on the ESP32's fractional 44.1 kHz BCLK, and a constant clock also
  // avoids the amp-faulting I2S reconfig on stream-rate changes.
  audio.setOutput48KHz(true);
  audio.setVolume(21);    // max soft volume for the test
  audio.connecttohost(kSpikeUrl);
}

void loop() {
  M5.update();
  audio.loop();

  // Output clock is pinned at 48 kHz — the amp keeps one config; the hook only
  // runs as fault recovery (SWS check) now, driven by the 5 s beat below.
  lastRate = audio.getSampleRate();

  static uint32_t nextBeat = 0;
  if (millis() > nextBeat) {
    nextBeat = millis() + 5000;
    Serial.printf("[m0] running=%d buf=%lu rate=%lu\n", audio.isRunning(),
                  (unsigned long)audio.inBufferFilled(), (unsigned long)lastRate);
    if (audio.isRunning() && audio.inBufferFilled() > 0) {
      dumpAmp("streaming");
      audio_out::onSampleRate(profile, 48000);  // recovers the amp if faulted
    }
  }
  vTaskDelay(1);
}
